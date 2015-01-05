//
// Copyright (c) 2008-2014 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "Precompiled.h"
#include "../Core/Context.h"
#include "../Core/CoreEvents.h"
#include "../IO/FileSystem.h"
#include "../Graphics/Graphics.h"
#include "../Graphics/GraphicsEvents.h"
#include "../Graphics/GraphicsImpl.h"
#include "../Input/Input.h"
#include "../IO/Log.h"
#include "../Core/Mutex.h"
#include "../Core/ProcessUtils.h"
#include "../Core/Profiler.h"
#include "../Resource/ResourceCache.h"
#include "../IO/RWOpsWrapper.h"
#include "../Core/StringUtils.h"
#include "../UI/Text.h"
#include "../UI/UI.h"

#include <cstring>

#include <SDL/SDL.h>

#include "../DebugNew.h"

extern "C" int SDL_AddTouch(SDL_TouchID touchID, const char *name);

// Require a click inside window before re-hiding mouse cursor on OSX, otherwise dragging the window
// can be incorrectly interpreted as mouse movement inside the window
#if defined(__APPLE__) && !defined(IOS)
    #define REQUIRE_CLICK_TO_FOCUS
#endif

namespace Urho3D
{

const int SCREEN_JOYSTICK_START_ID = 0x40000000;
const StringHash VAR_BUTTON_KEY_BINDING("VAR_BUTTON_KEY_BINDING");
const StringHash VAR_BUTTON_MOUSE_BUTTON_BINDING("VAR_BUTTON_MOUSE_BUTTON_BINDING");
const StringHash VAR_LAST_KEYSYM("VAR_LAST_KEYSYM");
const StringHash VAR_SCREEN_JOYSTICK_ID("VAR_SCREEN_JOYSTICK_ID");

const unsigned TOUCHID_MAX = 32;

/// Convert SDL keycode if necessary.
int ConvertSDLKeyCode(int keySym, int scanCode)
{
    if (scanCode == SCANCODE_AC_BACK)
        return KEY_ESC;
    else
        return SDL_toupper(keySym);
}

UIElement* TouchState::GetTouchedElement()
{
    return touchedElement_.Get();
}

void JoystickState::Initialize(unsigned numButtons, unsigned numAxes, unsigned numHats)
{
    buttons_.resize(numButtons);
    buttonPress_.resize(numButtons);
    axes_.resize(numAxes);
    hats_.resize(numHats);

    Reset();
}

void JoystickState::Reset()
{
    for (unsigned i = 0; i < buttons_.size(); ++i)
    {
        buttons_[i] = false;
        buttonPress_[i] = false;
    }
    for (unsigned i = 0; i < axes_.size(); ++i)
        axes_[i] = 0.0f;
    for (unsigned i = 0; i < hats_.size(); ++i)
        hats_[i] = HAT_CENTER;
}

Input::Input(Context* context) :
    Object(context),
    mouseButtonDown_(0),
    mouseButtonPress_(0),
    mouseMoveWheel_(0),
    windowID_(0),
    toggleFullscreen_(true),
    mouseVisible_(false),
    lastMouseVisible_(false),
    mouseGrabbed_(false),
    mouseMode_(MM_ABSOLUTE),
    lastVisibleMousePosition_(MOUSE_POSITION_OFFSCREEN),
    touchEmulation_(false),
    inputFocus_(false),
    minimized_(false),
    focusedThisFrame_(false),
    suppressNextMouseMove_(false),
    initialized_(false)
{
    for (int i = 0; i < TOUCHID_MAX; i++)
        availableTouchIDs_.push_back(i);

    SubscribeToEvent(E_SCREENMODE, HANDLER(Input, HandleScreenMode));

    // Try to initialize right now, but skip if screen mode is not yet set
    Initialize();
}

Input::~Input()
{
}

void Input::Update()
{
    assert(initialized_);

    PROFILE(UpdateInput);

    // Reset input accumulation for this frame
    keyPress_.clear();
    scancodePress_.clear();
    mouseButtonPress_ = 0;
    mouseMove_ = IntVector2::ZERO;
    mouseMoveWheel_ = 0;
    for (auto & elem : joysticks_)
    {
        for (unsigned j = 0; j < ELEMENT_VALUE(elem).buttonPress_.size(); ++j)
            ELEMENT_VALUE(elem).buttonPress_[j] = false;
    }

    // Reset touch delta movement
    for (auto & state : touches_)
    {
        ELEMENT_VALUE(state).lastPosition_ = ELEMENT_VALUE(state).position_;
        ELEMENT_VALUE(state).delta_ = IntVector2::ZERO;
    }

    // Check and handle SDL events
    SDL_PumpEvents();
    SDL_Event evt;
    while (SDL_PeepEvents(&evt, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) > 0)
        HandleSDLEvent(&evt);

    if (mouseVisible_ && mouseMode_ == MM_WRAP)
    {
        IntVector2 mpos;
        SDL_GetMouseState(&mpos.x_, &mpos.y_);

        int buffer = 5;
        int width = graphics_->GetWidth() - buffer * 2;
        int height = graphics_->GetHeight() - buffer * 2;

        bool warp = false;
        if (mpos.x_ < buffer)
        {
            warp = true;
            mpos.x_ += width;
        }

        if (mpos.x_ > buffer + width)
        {
            warp = true;
            mpos.x_ -= width;
        }

        if (mpos.y_ < buffer)
        {
            warp = true;
            mpos.y_ += height;
        }

        if (mpos.y_ > buffer + height)
        {
            warp = true;
            mpos.y_ -= height;
        }

        if (warp)
        {
            SetMousePosition(mpos);
            SDL_FlushEvent(SDL_MOUSEMOTION);
        }
    }

    // Check for activation and inactivation from SDL window flags. Must nullcheck the window pointer because it may have
    // been closed due to input events
    SDL_Window* window = graphics_->GetImpl()->GetWindow();
    unsigned flags = window ? SDL_GetWindowFlags(window) & (SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS) : 0;
    if (window)
    {
#ifdef REQUIRE_CLICK_TO_FOCUS
        if (!inputFocus_ && (graphics_->GetFullscreen() || mouseVisible_) && flags == (SDL_WINDOW_INPUT_FOCUS |
            SDL_WINDOW_MOUSE_FOCUS))
#else
        if (!inputFocus_ && (flags & SDL_WINDOW_INPUT_FOCUS))
#endif
            focusedThisFrame_ = true;

        if (focusedThisFrame_)
            GainFocus();

        if (inputFocus_ && (flags & SDL_WINDOW_INPUT_FOCUS) == 0)
            LoseFocus();
    }
    else
        return;

    // Check for relative mode mouse move
    if (!touchEmulation_ && (graphics_->GetExternalWindow() || (!mouseVisible_ && inputFocus_ && (flags & SDL_WINDOW_MOUSE_FOCUS))))
    {
        IntVector2 mousePosition = GetMousePosition();
        mouseMove_ = mousePosition - lastMousePosition_;

        if (graphics_->GetExternalWindow())
            lastMousePosition_ = mousePosition;
        else
        {
            // Recenter the mouse cursor manually after move
            IntVector2 center(graphics_->GetWidth() / 2, graphics_->GetHeight() / 2);
            if (mousePosition != center)
            {
                SetMousePosition(center);
                lastMousePosition_ = center;
            }
        }

        // Send mouse move event if necessary
        if (mouseMove_ != IntVector2::ZERO)
        {
            if (suppressNextMouseMove_)
            {
                mouseMove_ = IntVector2::ZERO;
                suppressNextMouseMove_ = false;
            }
            else
            {
                using namespace MouseMove;

                VariantMap& eventData = GetEventDataMap();
                if (mouseVisible_)
                {
                    eventData[P_X] = mousePosition.x_;
                    eventData[P_Y] = mousePosition.y_;
                }
                eventData[P_DX] = mouseMove_.x_;
                eventData[P_DY] = mouseMove_.y_;
                eventData[P_BUTTONS] = mouseButtonDown_;
                eventData[P_QUALIFIERS] = GetQualifiers();
                SendEvent(E_MOUSEMOVE, eventData);
            }
        }
    }
}

void Input::SetMouseVisible(bool enable, bool suppressEvent)
{
    // In touch emulation mode only enabled mouse is allowed
    if (touchEmulation_)
        enable = true;

    // In mouse mode relative, the mouse should be invisible
    if (mouseMode_ == MM_RELATIVE)
        enable = false;

    // SDL Raspberry Pi "video driver" does not have proper OS mouse support yet, so no-op for now
    #ifndef RPI
    if (enable != mouseVisible_)
    {
        mouseVisible_ = enable;

        if (initialized_)
        {
            // External windows can only support visible mouse cursor
            if (graphics_->GetExternalWindow())
            {
                mouseVisible_ = true;
                if (!suppressEvent)
                    lastMouseVisible_ = true;
                return;
            }

            if (!mouseVisible_ && inputFocus_)
            {
                SDL_ShowCursor(SDL_FALSE);
                // Recenter the mouse cursor manually when hiding it to avoid erratic mouse move for one frame
                lastVisibleMousePosition_ = GetMousePosition();
                IntVector2 center(graphics_->GetWidth() / 2, graphics_->GetHeight() / 2);
                SetMousePosition(center);
                lastMousePosition_ = center;
            }
            else
            {
                SDL_ShowCursor(SDL_TRUE);
                if (lastVisibleMousePosition_.x_ != MOUSE_POSITION_OFFSCREEN.x_ && lastVisibleMousePosition_.y_ != MOUSE_POSITION_OFFSCREEN.y_)
                    SetMousePosition(lastVisibleMousePosition_);
            }
        }

        if (!suppressEvent)
        {
            using namespace MouseVisibleChanged;

            VariantMap& eventData = GetEventDataMap();
            eventData[P_VISIBLE] = mouseVisible_;
            SendEvent(E_MOUSEVISIBLECHANGED, eventData);
        }
    }

    // Make sure last mouse visible is valid:
    if (!suppressEvent)
        lastMouseVisible_ = mouseVisible_;
    #endif
}

void Input::SetMouseGrabbed(bool grab)
{
    mouseGrabbed_ = grab;
}

void Input::SetMouseMode(MouseMode mode)
{
    if (mode != mouseMode_)
    {
        MouseMode previousMode = mouseMode_;
        mouseMode_ = mode;
        // Handle changing away from previous mode
        if (previousMode == MM_RELATIVE)
        {
            /// \todo Use SDL_SetRelativeMouseMode() for MM_RELATIVE mode
            ResetMouseVisible();

            // Send updated mouse position:
            using namespace MouseMove;

            VariantMap& eventData = GetEventDataMap();
            eventData[P_X] = lastVisibleMousePosition_.x_;
            eventData[P_Y] = lastVisibleMousePosition_.y_;
            eventData[P_DX] = mouseMove_.x_;
            eventData[P_DY] = mouseMove_.y_;
            eventData[P_BUTTONS] = mouseButtonDown_;
            eventData[P_QUALIFIERS] = GetQualifiers();
            SendEvent(E_MOUSEMOVE, eventData);
        }
        else if (previousMode == MM_WRAP)
        {
            SDL_Window* window = graphics_->GetImpl()->GetWindow();
            SDL_SetWindowGrab(window, SDL_FALSE);
        }

        // Handle changing to new mode
        if (mode == MM_ABSOLUTE)
            SetMouseGrabbed(false);
        else
        {
            SetMouseGrabbed(true);

            if (mode == MM_RELATIVE)
            {
                SetMouseVisible(false, true);
            }
            else if (mode == MM_WRAP)
            {
                /// \todo When SDL 2.0.4 is integrated, use SDL_CaptureMouse() and global mouse functions for MM_WRAP mode.
                SDL_Window* window = graphics_->GetImpl()->GetWindow();
                SDL_SetWindowGrab(window, SDL_TRUE);
            }
        }
    }
}

void Input::SetToggleFullscreen(bool enable)
{
    toggleFullscreen_ = enable;
}

static void PopulateKeyBindingMap(HashMap<String, int>& keyBindingMap)
{
    if (!keyBindingMap.isEmpty())
        return;
    keyBindingMap.emplace("SPACE", KEY_SPACE);
    keyBindingMap.emplace("LCTRL", KEY_LCTRL);
    keyBindingMap.emplace("RCTRL", KEY_RCTRL);
    keyBindingMap.emplace("LSHIFT", KEY_LSHIFT);
    keyBindingMap.emplace("RSHIFT", KEY_RSHIFT);
    keyBindingMap.emplace("LALT", KEY_LALT);
    keyBindingMap.emplace("RALT", KEY_RALT);
    keyBindingMap.emplace("LGUI", KEY_LGUI);
    keyBindingMap.emplace("RGUI", KEY_RGUI);
    keyBindingMap.emplace("TAB", KEY_TAB);
    keyBindingMap.emplace("RETURN", KEY_RETURN);
    keyBindingMap.emplace("RETURN2", KEY_RETURN2);
    keyBindingMap.emplace("ENTER", KEY_KP_ENTER);
    keyBindingMap.emplace("SELECT", KEY_SELECT);
    keyBindingMap.emplace("LEFT", KEY_LEFT);
    keyBindingMap.emplace("RIGHT", KEY_RIGHT);
    keyBindingMap.emplace("UP", KEY_UP);
    keyBindingMap.emplace("DOWN", KEY_DOWN);
    keyBindingMap.emplace("PAGEUP", KEY_PAGEUP);
    keyBindingMap.emplace("PAGEDOWN", KEY_PAGEDOWN);
    keyBindingMap.emplace("F1", KEY_F1);
    keyBindingMap.emplace("F2", KEY_F2);
    keyBindingMap.emplace("F3", KEY_F3);
    keyBindingMap.emplace("F4", KEY_F4);
    keyBindingMap.emplace("F5", KEY_F5);
    keyBindingMap.emplace("F6", KEY_F6);
    keyBindingMap.emplace("F7", KEY_F7);
    keyBindingMap.emplace("F8", KEY_F8);
    keyBindingMap.emplace("F9", KEY_F9);
    keyBindingMap.emplace("F10", KEY_F10);
    keyBindingMap.emplace("F11", KEY_F11);
    keyBindingMap.emplace("F12", KEY_F12);
}

static void PopulateMouseButtonBindingMap(HashMap<String, int>& mouseButtonBindingMap)
{
    if (!mouseButtonBindingMap.isEmpty())
        return;
    mouseButtonBindingMap.emplace("LEFT", SDL_BUTTON_LEFT);
    mouseButtonBindingMap.emplace("MIDDLE", SDL_BUTTON_MIDDLE);
    mouseButtonBindingMap.emplace("RIGHT", SDL_BUTTON_RIGHT);
    mouseButtonBindingMap.emplace("X1", SDL_BUTTON_X1);
    mouseButtonBindingMap.emplace("X2", SDL_BUTTON_X2);
}

SDL_JoystickID Input::AddScreenJoystick(XMLFile* layoutFile, XMLFile* styleFile)
{
    static HashMap<String, int> keyBindingMap;
    static HashMap<String, int> mouseButtonBindingMap;

    if (!graphics_)
    {
        LOGWARNING("Cannot add screen joystick in headless mode");
        return -1;
    }

    // If layout file is not given, use the default screen joystick layout
    if (!layoutFile)
    {
        ResourceCache* cache = GetSubsystem<ResourceCache>();
        layoutFile = cache->GetResource<XMLFile>("UI/ScreenJoystick.xml");
        if (!layoutFile)    // Error is already logged
            return -1;
    }

    UI* ui = GetSubsystem<UI>();
    SharedPtr<UIElement> screenJoystick = ui->LoadLayout(layoutFile, styleFile);
    if (!screenJoystick)     // Error is already logged
        return -1;

    screenJoystick->SetSize(ui->GetRoot()->GetSize());
    ui->GetRoot()->AddChild(screenJoystick);

    // Get an unused ID for the screen joystick
    /// \todo After a real joystick has been plugged in 1073741824 times, the ranges will overlap
    SDL_JoystickID joystickID = SCREEN_JOYSTICK_START_ID;
    while (joysticks_.contains(joystickID))
        ++joystickID;

    JoystickState& state = joysticks_[joystickID];
    state.joystickID_ = joystickID;
    state.name_ = screenJoystick->GetName();
    state.screenJoystick_ = screenJoystick;

    unsigned numButtons = 0;
    unsigned numAxes = 0;
    unsigned numHats = 0;
    const Vector<SharedPtr<UIElement> >& children = state.screenJoystick_->GetChildren();
    for (const auto & elem : children)
    {
        UIElement* element = elem.Get();
        String name = element->GetName();
        if (name.startsWith("Button"))
        {
            ++numButtons;

            // Check whether the button has key binding
            Text* text = dynamic_cast<Text*>(element->GetChild("KeyBinding", false));
            if (text)
            {
                text->SetVisible(false);
                const String& key = text->GetText();
                int keyBinding;
                if (key.length() == 1)
                    keyBinding = key[0];
                else
                {
                    PopulateKeyBindingMap(keyBindingMap);

                    auto i = keyBindingMap.find(key);
                    if (i != keyBindingMap.end())
                        keyBinding = MAP_VALUE(i);
                    else
                    {
                        LOGERRORF("Unsupported key binding: %s", key.CString());
                        keyBinding = M_MAX_INT;
                    }
                }

                if (keyBinding != M_MAX_INT)
                    element->SetVar(VAR_BUTTON_KEY_BINDING, keyBinding);
            }

            // Check whether the button has mouse button binding
            text = dynamic_cast<Text*>(element->GetChild("MouseButtonBinding", false));
            if (text)
            {
                text->SetVisible(false);
                const String& mouseButton = text->GetText();
                PopulateMouseButtonBindingMap(mouseButtonBindingMap);

                auto i = mouseButtonBindingMap.find(mouseButton);
                if (i != mouseButtonBindingMap.end())
                    element->SetVar(VAR_BUTTON_MOUSE_BUTTON_BINDING, MAP_VALUE(i));
                else
                    LOGERRORF("Unsupported mouse button binding: %s", mouseButton.CString());
            }
        }
        else if (name.startsWith("Axis"))
        {
            ++numAxes;

            ///\todo Axis emulation for screen joystick is not fully supported yet.
            LOGWARNING("Axis emulation for screen joystick is not fully supported yet");
        }
        else if (name.startsWith("Hat"))
        {
            ++numHats;

            Text* text = dynamic_cast<Text*>(element->GetChild("KeyBinding", false));
            if (text)
            {
                text->SetVisible(false);
                String keyBinding = text->GetText();
                if (keyBinding.contains(' '))   // e.g.: "UP DOWN LEFT RIGHT"
                {
                    // Attempt to split the text using ' ' as separator
                    Vector<String>keyBindings(keyBinding.split(' '));
                    String mappedKeyBinding;
                    if (keyBindings.size() == 4)
                    {
                        PopulateKeyBindingMap(keyBindingMap);

                        for (unsigned j = 0; j < 4; ++j)
                        {
                            if (keyBindings[j].length() == 1)
                                mappedKeyBinding.append(keyBindings[j][0]);
                            else
                            {
                                auto i = keyBindingMap.find(keyBindings[j]);
                                if (i != keyBindingMap.end())
                                    mappedKeyBinding.append(MAP_VALUE(i));
                                else
                                    break;
                            }
                        }
                    }
                    if (mappedKeyBinding.length() != 4)
                    {
                        LOGERRORF("%s has invalid key binding %s, fallback to WSAD", name.CString(), keyBinding.CString());
                        keyBinding = "WSAD";
                    }
                    else
                        keyBinding = mappedKeyBinding;
                }
                else if (keyBinding.length() != 4)
                {
                    LOGERRORF("%s has invalid key binding %s, fallback to WSAD", name.CString(), keyBinding.CString());
                    keyBinding = "WSAD";
                }

                element->SetVar(VAR_BUTTON_KEY_BINDING, keyBinding);
            }
        }

        element->SetVar(VAR_SCREEN_JOYSTICK_ID, joystickID);
    }

    // Make sure all the children are non-focusable so they do not mistakenly to be considered as active UI input controls by application
    PODVector<UIElement*> allChildren;
    state.screenJoystick_->GetChildren(allChildren, true);
    for (auto & elem : allChildren)
        (elem)->SetFocusMode(FM_NOTFOCUSABLE);

    state.Initialize(numButtons, numAxes, numHats);

    // There could be potentially more than one screen joystick, however they all will be handled by a same handler method
    // So there is no harm to replace the old handler with the new handler in each call to SubscribeToEvent()
    SubscribeToEvent(E_TOUCHBEGIN, HANDLER(Input, HandleScreenJoystickTouch));
    SubscribeToEvent(E_TOUCHMOVE, HANDLER(Input, HandleScreenJoystickTouch));
    SubscribeToEvent(E_TOUCHEND, HANDLER(Input, HandleScreenJoystickTouch));

    return joystickID;
}

bool Input::RemoveScreenJoystick(SDL_JoystickID id)
{
    if (!joysticks_.contains(id))
    {
        LOGERRORF("Failed to remove non-existing screen joystick ID #%d", id);
        return false;
    }

    JoystickState& state = joysticks_[id];
    if (!state.screenJoystick_)
    {
        LOGERRORF("Failed to remove joystick with ID #%d which is not a screen joystick", id);
        return false;
    }

    state.screenJoystick_->Remove();
    joysticks_.remove(id);

    return true;
}

void Input::SetScreenJoystickVisible(SDL_JoystickID id, bool enable)
{
    if (joysticks_.contains(id))
    {
        JoystickState& state = joysticks_[id];

        if (state.screenJoystick_)
            state.screenJoystick_->SetVisible(enable);
    }
}

void Input::SetScreenKeyboardVisible(bool enable)
{
    if (!graphics_)
        return;

    if (enable != IsScreenKeyboardVisible())
    {
        if (enable)
            SDL_StartTextInput();
        else
            SDL_StopTextInput();
    }
}

void Input::SetTouchEmulation(bool enable)
{
#if !defined(ANDROID) && !defined(IOS)
    if (enable != touchEmulation_)
    {
        if (enable)
        {
            // Touch emulation needs the mouse visible
            if (!mouseVisible_)
                SetMouseVisible(true);

            // Add a virtual touch device the first time we are enabling emulated touch
            if (!SDL_GetNumTouchDevices())
                SDL_AddTouch(0, "Emulated Touch");
        }
        else
            ResetTouches();

        touchEmulation_ = enable;
    }
#endif
}

bool Input::RecordGesture()
{
    // If have no touch devices, fail
    if (!SDL_GetNumTouchDevices())
    {
        LOGERROR("Can not record gesture: no touch devices");
        return false;
    }

    return SDL_RecordGesture(-1) != 0;
}

bool Input::SaveGestures(Serializer& dest)
{
    RWOpsWrapper<Serializer> wrapper(dest);
    return SDL_SaveAllDollarTemplates(wrapper.GetRWOps()) != 0;
}

bool Input::SaveGesture(Serializer& dest, unsigned gestureID)
{
    RWOpsWrapper<Serializer> wrapper(dest);
    return SDL_SaveDollarTemplate(gestureID, wrapper.GetRWOps()) != 0;
}

unsigned Input::LoadGestures(Deserializer& source)
{
    // If have no touch devices, fail
    if (!SDL_GetNumTouchDevices())
    {
        LOGERROR("Can not load gestures: no touch devices");
        return 0;
    }

    RWOpsWrapper<Deserializer> wrapper(source);
    return SDL_LoadDollarTemplates(-1, wrapper.GetRWOps());
}

bool Input::RemoveGesture(unsigned gestureID)
{
    return SDL_RemoveDollarTemplate(gestureID) != 0;
}

void Input::RemoveAllGestures()
{
    SDL_RemoveAllDollarTemplates();
}

SDL_JoystickID Input::OpenJoystick(unsigned index)
{
    SDL_Joystick* joystick = SDL_JoystickOpen(index);
    if (!joystick)
    {
        LOGERRORF("Cannot open joystick #%d", index);
        return -1;
    }

    // Create joystick state for the new joystick
    int joystickID = SDL_JoystickInstanceID(joystick);
    JoystickState& state = joysticks_[joystickID];
    state.joystick_ = joystick;
    state.joystickID_ = joystickID;
    state.name_ = SDL_JoystickName(joystick);
    if (SDL_IsGameController(index))
       state.controller_ = SDL_GameControllerOpen(index);

    unsigned numButtons = SDL_JoystickNumButtons(joystick);
    unsigned numAxes = SDL_JoystickNumAxes(joystick);
    unsigned numHats = SDL_JoystickNumHats(joystick);

    // When the joystick is a controller, make sure there's enough axes & buttons for the standard controller mappings
    if (state.controller_)
    {
        if (numButtons < SDL_CONTROLLER_BUTTON_MAX)
            numButtons = SDL_CONTROLLER_BUTTON_MAX;
        if (numAxes < SDL_CONTROLLER_AXIS_MAX)
            numAxes = SDL_CONTROLLER_AXIS_MAX;
    }

    state.Initialize(numButtons, numAxes, numHats);

    return joystickID;
}

int Input::GetKeyFromName(const String& name) const
{
    return SDL_GetKeyFromName(name.CString());
}

int Input::GetKeyFromScancode(int scancode) const
{
    return SDL_GetKeyFromScancode((SDL_Scancode)scancode);
}

String Input::GetKeyName(int key) const
{
    return String(SDL_GetKeyName(key));
}

int Input::GetScancodeFromKey(int key) const
{
    return SDL_GetScancodeFromKey(key);
}

int Input::GetScancodeFromName(const String& name) const
{
    return SDL_GetScancodeFromName(name.CString());
}

String Input::GetScancodeName(int scancode) const
{
    return SDL_GetScancodeName((SDL_Scancode)scancode);
}

bool Input::GetKeyDown(int key) const
{
    return keyDown_.contains(SDL_toupper(key));
}

bool Input::GetKeyPress(int key) const
{
    return keyPress_.contains(SDL_toupper(key));
}

bool Input::GetScancodeDown(int scancode) const
{
    return scancodeDown_.contains(scancode);
}

bool Input::GetScancodePress(int scancode) const
{
    return scancodePress_.contains(scancode);
}

bool Input::GetMouseButtonDown(int button) const
{
    return (mouseButtonDown_ & button) != 0;
}

bool Input::GetMouseButtonPress(int button) const
{
    return (mouseButtonPress_ & button) != 0;
}

bool Input::GetQualifierDown(int qualifier) const
{
    if (qualifier == QUAL_SHIFT)
        return GetKeyDown(KEY_LSHIFT) || GetKeyDown(KEY_RSHIFT);
    if (qualifier == QUAL_CTRL)
        return GetKeyDown(KEY_LCTRL) || GetKeyDown(KEY_RCTRL);
    if (qualifier == QUAL_ALT)
        return GetKeyDown(KEY_LALT) || GetKeyDown(KEY_RALT);

    return false;
}

bool Input::GetQualifierPress(int qualifier) const
{
    if (qualifier == QUAL_SHIFT)
        return GetKeyPress(KEY_LSHIFT) || GetKeyPress(KEY_RSHIFT);
    if (qualifier == QUAL_CTRL)
        return GetKeyPress(KEY_LCTRL) || GetKeyPress(KEY_RCTRL);
    if (qualifier == QUAL_ALT)
        return GetKeyPress(KEY_LALT) || GetKeyPress(KEY_RALT);

    return false;
}

int Input::GetQualifiers() const
{
    int ret = 0;
    if (GetQualifierDown(QUAL_SHIFT))
        ret |= QUAL_SHIFT;
    if (GetQualifierDown(QUAL_CTRL))
        ret |= QUAL_CTRL;
    if (GetQualifierDown(QUAL_ALT))
        ret |= QUAL_ALT;

    return ret;
}

IntVector2 Input::GetMousePosition() const
{
    IntVector2 ret = IntVector2::ZERO;

    if (!initialized_)
        return ret;

    SDL_GetMouseState(&ret.x_, &ret.y_);

    return ret;
}

TouchState* Input::GetTouch(unsigned index) const
{
    if (index >= touches_.size())
        return nullptr;

    auto i = touches_.begin();
    while (index--)
        ++i;

    return const_cast<TouchState*>(&MAP_VALUE(i));
}

JoystickState* Input::GetJoystickByIndex(unsigned index)
{
    unsigned compare = 0;
    for (auto & elem : joysticks_)
    {
        if (compare++ == index)
            return &ELEMENT_VALUE(elem);
    }

    return nullptr;
}

JoystickState* Input::GetJoystick(SDL_JoystickID id)
{
    auto i = joysticks_.find(id);
    return i != joysticks_.end() ? &MAP_VALUE(i) : nullptr;
}

bool Input::IsScreenJoystickVisible(SDL_JoystickID id) const
{
    auto i = joysticks_.find(id);
    return i != joysticks_.end() && MAP_VALUE(i).screenJoystick_ && MAP_VALUE(i).screenJoystick_->IsVisible();
}

bool Input::GetScreenKeyboardSupport() const
{
    return graphics_ ? SDL_HasScreenKeyboardSupport() != 0 : false;
}

bool Input::IsScreenKeyboardVisible() const
{
    if (graphics_)
    {
        SDL_Window* window = graphics_->GetImpl()->GetWindow();
        return SDL_IsScreenKeyboardShown(window) != SDL_FALSE;
    }
    else
        return false;
}

bool Input::IsMinimized() const
{
    // Return minimized state also when unfocused in fullscreen
    if (!inputFocus_ && graphics_ && graphics_->GetFullscreen())
        return true;
    else
        return minimized_;
}

void Input::Initialize()
{
    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics || !graphics->IsInitialized())
        return;

    graphics_ = graphics;

    // In external window mode only visible mouse is supported
    if (graphics_->GetExternalWindow())
        mouseVisible_ = true;

    // Set the initial activation
    focusedThisFrame_ = true;
    initialized_ = true;

    ResetJoysticks();
    ResetState();

    SubscribeToEvent(E_BEGINFRAME, HANDLER(Input, HandleBeginFrame));

    LOGINFO("Initialized input");
}

void Input::ResetJoysticks()
{
    joysticks_.clear();

    // Open each detected joystick automatically on startup
    int size = SDL_NumJoysticks();
    for (int i = 0; i < size; ++i)
        OpenJoystick(i);
}

void Input::GainFocus()
{
    ResetState();

    inputFocus_ = true;
    focusedThisFrame_ = false;

    // Restore mouse mode
    MouseMode mm = mouseMode_;
    mouseMode_ = MM_ABSOLUTE;
    SetMouseMode(mm);

    // Re-establish mouse cursor hiding as necessary
    if (!mouseVisible_)
    {
        SDL_ShowCursor(SDL_FALSE);
        suppressNextMouseMove_ = true;
    }
    else
        lastMousePosition_ = GetMousePosition();

    SendInputFocusEvent();
}

void Input::LoseFocus()
{
    ResetState();

    inputFocus_ = false;
    focusedThisFrame_ = false;

    MouseMode mm = mouseMode_;

    // Show the mouse cursor when inactive
    SDL_ShowCursor(SDL_TRUE);

    // Change mouse mode -- removing any cursor grabs, etc.
    SetMouseMode(MM_ABSOLUTE);

    // Restore flags to reflect correct mouse state.
    mouseMode_ = mm;

    SendInputFocusEvent();
}

void Input::ResetState()
{
    keyDown_.clear();
    keyPress_.clear();
    scancodeDown_.clear();
    scancodePress_.clear();

    /// \todo Check if resetting joystick state on input focus loss is even necessary
    for (auto & elem : joysticks_)
        ELEMENT_VALUE(elem).Reset();

    ResetTouches();

    // Use SetMouseButton() to reset the state so that mouse events will be sent properly
    SetMouseButton(MOUSEB_LEFT, false);
    SetMouseButton(MOUSEB_RIGHT, false);
    SetMouseButton(MOUSEB_MIDDLE, false);

    mouseMove_ = IntVector2::ZERO;
    mouseMoveWheel_ = 0;
    mouseButtonPress_ = 0;
}

void Input::ResetTouches()
{
    for (auto & elem : touches_)
    {
        TouchState & state(ELEMENT_VALUE(elem));
        using namespace TouchEnd;

        VariantMap& eventData = GetEventDataMap();
        eventData[P_TOUCHID] = state.touchID_;
        eventData[P_X] = state.position_.x_;
        eventData[P_Y] = state.position_.y_;
        SendEvent(E_TOUCHEND, eventData);
    }

    touches_.clear();
    touchIDMap_.clear();
    availableTouchIDs_.clear();
    for (int i = 0; i < TOUCHID_MAX; i++)
        availableTouchIDs_.push_back(i);

}

unsigned Input::GetTouchIndexFromID(int touchID)
{
    auto i = touchIDMap_.find(touchID);
    if (i != touchIDMap_.end())
    {
        return MAP_VALUE(i);
    }

    int index = PopTouchIndex();
    touchIDMap_[touchID] = index;
    return index;
}

unsigned Input::PopTouchIndex()
{
    if (availableTouchIDs_.isEmpty())
        return 0;

    unsigned index = availableTouchIDs_.front();
    availableTouchIDs_.pop_front();
    return index;
}

void Input::PushTouchIndex(int touchID)
{
    if (!touchIDMap_.contains(touchID))
        return;

    int index = touchIDMap_[touchID];
    touchIDMap_.remove(touchID);

    // Sorted insertion
    bool inserted = false;
    for (QList<int>::Iterator i = availableTouchIDs_.begin(); i != availableTouchIDs_.end(); ++i)
    {
        if (*i == index)
        {
            // This condition can occur when TOUCHID_MAX is reached.
            inserted = true;
            break;
        }

        if (*i > index)
        {
            availableTouchIDs_.insert(i, index);
            inserted = true;
            break;
        }
    }

    // If empty, or the lowest value then insert at end.
    if (!inserted)
        availableTouchIDs_.push_back(index);
}

void Input::SendInputFocusEvent()
{
    using namespace InputFocus;

    VariantMap& eventData = GetEventDataMap();
    eventData[P_FOCUS] = HasFocus();
    eventData[P_MINIMIZED] = IsMinimized();
    SendEvent(E_INPUTFOCUS, eventData);
}

void Input::SetMouseButton(int button, bool newState)
{
#ifdef REQUIRE_CLICK_TO_FOCUS
    if (!mouseVisible_ && !graphics_->GetFullscreen())
    {
        if (!inputFocus_ && newState && button == MOUSEB_LEFT)
            focusedThisFrame_ = true;
    }
#endif

    // If we do not have focus yet, do not react to the mouse button down
    if (!graphics_->GetExternalWindow() && newState && !inputFocus_)
        return;

    if (newState)
    {
        if (!(mouseButtonDown_ & button))
            mouseButtonPress_ |= button;

        mouseButtonDown_ |= button;
    }
    else
    {
        if (!(mouseButtonDown_ & button))
            return;

        mouseButtonDown_ &= ~button;
    }

    using namespace MouseButtonDown;

    VariantMap& eventData = GetEventDataMap();
    eventData[P_BUTTON] = button;
    eventData[P_BUTTONS] = mouseButtonDown_;
    eventData[P_QUALIFIERS] = GetQualifiers();
    SendEvent(newState ? E_MOUSEBUTTONDOWN : E_MOUSEBUTTONUP, eventData);
}

void Input::SetKey(int key, int scancode, unsigned raw, bool newState)
{
    // If we do not have focus yet, do not react to the key down
    if (!graphics_->GetExternalWindow() && newState && !inputFocus_)
        return;

    bool repeat = false;

    if (newState)
    {
        scancodeDown_.insert(scancode);
        scancodePress_.insert(scancode);

        if (!keyDown_.contains(key))
        {
            keyDown_.insert(key);
            keyPress_.insert(key);
        }
        else
            repeat = true;
    }
    else
    {
        scancodeDown_.remove(scancode);

        if (!keyDown_.remove(key))
            return;
    }

    using namespace KeyDown;

    VariantMap& eventData = GetEventDataMap();
    eventData[P_KEY] = key;
    eventData[P_SCANCODE] = scancode;
    eventData[P_RAW] = raw;
    eventData[P_BUTTONS] = mouseButtonDown_;
    eventData[P_QUALIFIERS] = GetQualifiers();
    if (newState)
        eventData[P_REPEAT] = repeat;
    SendEvent(newState ? E_KEYDOWN : E_KEYUP, eventData);

    if ((key == KEY_RETURN || key == KEY_RETURN2 || key == KEY_KP_ENTER) && newState && !repeat && toggleFullscreen_ &&
        (GetKeyDown(KEY_LALT) || GetKeyDown(KEY_RALT)))
        graphics_->ToggleFullscreen();
}

void Input::SetMouseWheel(int delta)
{
    // If we do not have focus yet, do not react to the wheel
    if (!graphics_->GetExternalWindow() && !inputFocus_)
        return;

    if (delta)
    {
        mouseMoveWheel_ += delta;

        using namespace MouseWheel;

        VariantMap& eventData = GetEventDataMap();
        eventData[P_WHEEL] = delta;
        eventData[P_BUTTONS] = mouseButtonDown_;
        eventData[P_QUALIFIERS] = GetQualifiers();
        SendEvent(E_MOUSEWHEEL, eventData);
    }
}

void Input::SetMousePosition(const IntVector2& position)
{
    if (!graphics_)
        return;

    SDL_WarpMouseInWindow(graphics_->GetImpl()->GetWindow(), position.x_, position.y_);
}

void Input::HandleSDLEvent(void* sdlEvent)
{
    SDL_Event& evt = *static_cast<SDL_Event*>(sdlEvent);

    switch (evt.type)
    {
    case SDL_KEYDOWN:
        // Convert to uppercase to match Win32 virtual key codes
        SetKey(ConvertSDLKeyCode(evt.key.keysym.sym, evt.key.keysym.scancode), evt.key.keysym.scancode, evt.key.keysym.raw, true);
        break;

    case SDL_KEYUP:
        SetKey(ConvertSDLKeyCode(evt.key.keysym.sym, evt.key.keysym.scancode), evt.key.keysym.scancode, evt.key.keysym.raw, false);
        break;

    case SDL_TEXTINPUT:
        {
            textInput_ = &evt.text.text[0];
            unsigned unicode = textInput_.AtUTF8(0);
            if (unicode)
            {
                using namespace TextInput;

                VariantMap textInputEventData;

                textInputEventData[P_TEXT] = textInput_;
                textInputEventData[P_BUTTONS] = mouseButtonDown_;
                textInputEventData[P_QUALIFIERS] = GetQualifiers();
                SendEvent(E_TEXTINPUT, textInputEventData);
            }
        }
        break;

    case SDL_MOUSEBUTTONDOWN:
        if (!touchEmulation_)
            SetMouseButton(1 << (evt.button.button - 1), true);
        else
        {
            int x, y;
            SDL_GetMouseState(&x, &y);

            SDL_Event event;
            event.type = SDL_FINGERDOWN;
            event.tfinger.touchId = 0;
            event.tfinger.fingerId = evt.button.button - 1;
            event.tfinger.pressure = 1.0f;
            event.tfinger.x = (float)x / (float)graphics_->GetWidth();
            event.tfinger.y = (float)y / (float)graphics_->GetHeight();
            event.tfinger.dx = 0;
            event.tfinger.dy = 0;
            SDL_PushEvent(&event);
        }
        break;

    case SDL_MOUSEBUTTONUP:
        if (!touchEmulation_)
            SetMouseButton(1 << (evt.button.button - 1), false);
        else
        {
            int x, y;
            SDL_GetMouseState(&x, &y);

            SDL_Event event;
            event.type = SDL_FINGERUP;
            event.tfinger.touchId = 0;
            event.tfinger.fingerId = evt.button.button - 1;
            event.tfinger.pressure = 0.0f;
            event.tfinger.x = (float)x / (float)graphics_->GetWidth();
            event.tfinger.y = (float)y / (float)graphics_->GetHeight();
            event.tfinger.dx = 0;
            event.tfinger.dy = 0;
            SDL_PushEvent(&event);
        }
        break;

    case SDL_MOUSEMOTION:
        if ((mouseVisible_ || mouseMode_ == MM_RELATIVE) && !touchEmulation_)
        {
            mouseMove_.x_ += evt.motion.xrel;
            mouseMove_.y_ += evt.motion.yrel;

            using namespace MouseMove;

            VariantMap& eventData = GetEventDataMap();
            if (mouseVisible_)
            {
                eventData[P_X] = evt.motion.x;
                eventData[P_Y] = evt.motion.y;
            }
            eventData[P_DX] = evt.motion.xrel;
            eventData[P_DY] = evt.motion.yrel;
            eventData[P_BUTTONS] = mouseButtonDown_;
            eventData[P_QUALIFIERS] = GetQualifiers();
            SendEvent(E_MOUSEMOVE, eventData);
        }
        // Only the left mouse button "finger" moves along with the mouse movement
        else if (touchEmulation_ && touches_.contains(0))
        {
            int x, y;
            SDL_GetMouseState(&x, &y);

            SDL_Event event;
            event.type = SDL_FINGERMOTION;
            event.tfinger.touchId = 0;
            event.tfinger.fingerId = 0;
            event.tfinger.pressure = 1.0f;
            event.tfinger.x = (float)x / (float)graphics_->GetWidth();
            event.tfinger.y = (float)y / (float)graphics_->GetHeight();
            event.tfinger.dx = (float)evt.motion.xrel / (float)graphics_->GetWidth();
            event.tfinger.dy = (float)evt.motion.yrel / (float)graphics_->GetHeight();
            SDL_PushEvent(&event);
        }
        break;

    case SDL_MOUSEWHEEL:
        if (!touchEmulation_)
            SetMouseWheel(evt.wheel.y);
        break;

    case SDL_FINGERDOWN:
        if (evt.tfinger.touchId != SDL_TOUCH_MOUSEID)
        {
            int touchID = GetTouchIndexFromID(evt.tfinger.fingerId & 0x7ffffff);
            TouchState& state = touches_[touchID];
            state.touchID_ = touchID;
            state.lastPosition_ = state.position_ = IntVector2((int)(evt.tfinger.x * graphics_->GetWidth()),
                (int)(evt.tfinger.y * graphics_->GetHeight()));
            state.delta_ = IntVector2::ZERO;
            state.pressure_ = evt.tfinger.pressure;

            using namespace TouchBegin;

            VariantMap& eventData = GetEventDataMap();
            eventData[P_TOUCHID] = touchID;
            eventData[P_X] = state.position_.x_;
            eventData[P_Y] = state.position_.y_;
            eventData[P_PRESSURE] = state.pressure_;
            SendEvent(E_TOUCHBEGIN, eventData);
        }
        break;

    case SDL_FINGERUP:
        if (evt.tfinger.touchId != SDL_TOUCH_MOUSEID)
        {
            int touchID = GetTouchIndexFromID(evt.tfinger.fingerId & 0x7ffffff);
            TouchState& state = touches_[touchID];

            using namespace TouchEnd;

            VariantMap& eventData = GetEventDataMap();
            // Do not trust the position in the finger up event. Instead use the last position stored in the
            // touch structure
            eventData[P_TOUCHID] = touchID;
            eventData[P_X] = state.position_.x_;
            eventData[P_Y] = state.position_.y_;
            SendEvent(E_TOUCHEND, eventData);

            // Add touch index back to list of available touch Ids
            PushTouchIndex(evt.tfinger.fingerId & 0x7ffffff);

            touches_.remove(touchID);
        }
        break;

    case SDL_FINGERMOTION:
        if (evt.tfinger.touchId != SDL_TOUCH_MOUSEID)
        {
            int touchID = GetTouchIndexFromID(evt.tfinger.fingerId & 0x7ffffff);
            // We don't want this event to create a new touches_ event if it doesn't exist (touchEmulation)
            if (touchEmulation_ && !touches_.contains(touchID))
                break;
            TouchState& state = touches_[touchID];
            state.touchID_ = touchID;
            state.position_ = IntVector2((int)(evt.tfinger.x * graphics_->GetWidth()),
                (int)(evt.tfinger.y * graphics_->GetHeight()));
            state.delta_ = state.position_ - state.lastPosition_;
            state.pressure_ = evt.tfinger.pressure;

            using namespace TouchMove;

            VariantMap& eventData = GetEventDataMap();
            eventData[P_TOUCHID] = touchID;
            eventData[P_X] = state.position_.x_;
            eventData[P_Y] = state.position_.y_;
            eventData[P_DX] = (int)(evt.tfinger.dx * graphics_->GetWidth());
            eventData[P_DY] = (int)(evt.tfinger.dy * graphics_->GetHeight());
            eventData[P_PRESSURE] = state.pressure_;
            SendEvent(E_TOUCHMOVE, eventData);
        }
        break;

    case SDL_DOLLARRECORD:
        {
            using namespace GestureRecorded;

            VariantMap& eventData = GetEventDataMap();
            eventData[P_GESTUREID] = (int)evt.dgesture.gestureId;
            SendEvent(E_GESTURERECORDED, eventData);
        }
        break;

    case SDL_DOLLARGESTURE:
        {
            using namespace GestureInput;

            VariantMap& eventData = GetEventDataMap();
            eventData[P_GESTUREID] = (int)evt.dgesture.gestureId;
            eventData[P_CENTERX] = (int)(evt.dgesture.x * graphics_->GetWidth());
            eventData[P_CENTERY] = (int)(evt.dgesture.y * graphics_->GetHeight());
            eventData[P_NUMFINGERS] = (int)evt.dgesture.numFingers;
            eventData[P_ERROR] = evt.dgesture.error;
            SendEvent(E_GESTUREINPUT, eventData);
        }
        break;

    case SDL_MULTIGESTURE:
        {
            using namespace MultiGesture;

            VariantMap& eventData = GetEventDataMap();
            eventData[P_CENTERX] = (int)(evt.mgesture.x * graphics_->GetWidth());
            eventData[P_CENTERY] = (int)(evt.mgesture.y * graphics_->GetHeight());
            eventData[P_NUMFINGERS] = (int)evt.mgesture.numFingers;
            eventData[P_DTHETA] = M_RADTODEG * evt.mgesture.dTheta;
            eventData[P_DDIST] = evt.mgesture.dDist;
            SendEvent(E_MULTIGESTURE, eventData);
        }
        break;

    case SDL_JOYDEVICEADDED:
        {
            using namespace JoystickConnected;

            SDL_JoystickID joystickID = OpenJoystick(evt.jdevice.which);

            VariantMap& eventData = GetEventDataMap();
            eventData[P_JOYSTICKID] = joystickID;
            SendEvent(E_JOYSTICKCONNECTED, eventData);
        }
        break;

    case SDL_JOYDEVICEREMOVED:
        {
            using namespace JoystickDisconnected;

            joysticks_.remove(evt.jdevice.which);

            VariantMap& eventData = GetEventDataMap();
            eventData[P_JOYSTICKID] = evt.jdevice.which;
            SendEvent(E_JOYSTICKDISCONNECTED, eventData);
        }
        break;

    case SDL_JOYBUTTONDOWN:
        {
            using namespace JoystickButtonDown;

            unsigned button = evt.jbutton.button;
            SDL_JoystickID joystickID = evt.jbutton.which;
            JoystickState& state = joysticks_[joystickID];

            // Skip ordinary joystick event for a controller
            if (!state.controller_)
            {
                VariantMap& eventData = GetEventDataMap();
                eventData[P_JOYSTICKID] = joystickID;
                eventData[P_BUTTON] = button;

                if (button < state.buttons_.size())
                {
                    state.buttons_[button] = true;
                    state.buttonPress_[button] = true;
                    SendEvent(E_JOYSTICKBUTTONDOWN, eventData);
                }
            }
        }
        break;

    case SDL_JOYBUTTONUP:
        {
            using namespace JoystickButtonUp;

            unsigned button = evt.jbutton.button;
            SDL_JoystickID joystickID = evt.jbutton.which;
            JoystickState& state = joysticks_[joystickID];

            if (!state.controller_)
            {
                VariantMap& eventData = GetEventDataMap();
                eventData[P_JOYSTICKID] = joystickID;
                eventData[P_BUTTON] = button;

                if (button < state.buttons_.size())
                {
                    if (!state.controller_)
                        state.buttons_[button] = false;
                    SendEvent(E_JOYSTICKBUTTONUP, eventData);
                }
            }
        }
        break;

    case SDL_JOYAXISMOTION:
        {
            using namespace JoystickAxisMove;

            SDL_JoystickID joystickID = evt.jaxis.which;
            JoystickState& state = joysticks_[joystickID];

            if (!state.controller_)
            {
                VariantMap& eventData = GetEventDataMap();
                eventData[P_JOYSTICKID] = joystickID;
                eventData[P_AXIS] = evt.jaxis.axis;
                eventData[P_POSITION] = Clamp((float)evt.jaxis.value / 32767.0f, -1.0f, 1.0f);

                if (evt.jaxis.axis < state.axes_.size())
                {
                    // If the joystick is a controller, only use the controller axis mappings
                    // (we'll also get the controller event)
                    if (!state.controller_)
                        state.axes_[evt.jaxis.axis] = eventData[P_POSITION].GetFloat();
                    SendEvent(E_JOYSTICKAXISMOVE, eventData);
                }
            }
        }
        break;

    case SDL_JOYHATMOTION:
        {
            using namespace JoystickHatMove;

            SDL_JoystickID joystickID = evt.jaxis.which;
            JoystickState& state = joysticks_[joystickID];

            VariantMap& eventData = GetEventDataMap();
            eventData[P_JOYSTICKID] = joystickID;
            eventData[P_HAT] = evt.jhat.hat;
            eventData[P_POSITION] = evt.jhat.value;

            if (evt.jhat.hat < state.hats_.size())
            {
                state.hats_[evt.jhat.hat] = evt.jhat.value;
                SendEvent(E_JOYSTICKHATMOVE, eventData);
            }
        }
        break;

    case SDL_CONTROLLERBUTTONDOWN:
        {
            using namespace JoystickButtonDown;

            unsigned button = evt.cbutton.button;
            SDL_JoystickID joystickID = evt.cbutton.which;
            JoystickState& state = joysticks_[joystickID];

            VariantMap& eventData = GetEventDataMap();
            eventData[P_JOYSTICKID] = joystickID;
            eventData[P_BUTTON] = button;

            if (button < state.buttons_.size())
            {
                state.buttons_[button] = true;
                state.buttonPress_[button] = true;
                SendEvent(E_JOYSTICKBUTTONDOWN, eventData);
            }
        }
        break;

    case SDL_CONTROLLERBUTTONUP:
        {
            using namespace JoystickButtonUp;

            unsigned button = evt.cbutton.button;
            SDL_JoystickID joystickID = evt.cbutton.which;
            JoystickState& state = joysticks_[joystickID];

            VariantMap& eventData = GetEventDataMap();
            eventData[P_JOYSTICKID] = joystickID;
            eventData[P_BUTTON] = button;

            if (button < state.buttons_.size())
            {
                state.buttons_[button] = false;
                SendEvent(E_JOYSTICKBUTTONUP, eventData);
            }
        }
        break;

    case SDL_CONTROLLERAXISMOTION:
        {
            using namespace JoystickAxisMove;

            SDL_JoystickID joystickID = evt.caxis.which;
            JoystickState& state = joysticks_[joystickID];

            VariantMap& eventData = GetEventDataMap();
            eventData[P_JOYSTICKID] = joystickID;
            eventData[P_AXIS] = evt.caxis.axis;
            eventData[P_POSITION] = Clamp((float)evt.caxis.value / 32767.0f, -1.0f, 1.0f);

            if (evt.caxis.axis < state.axes_.size())
            {
                state.axes_[evt.caxis.axis] = eventData[P_POSITION].GetFloat();
                SendEvent(E_JOYSTICKAXISMOVE, eventData);
            }
        }
        break;

    case SDL_WINDOWEVENT:
        {
            switch (evt.window.event)
            {
            case SDL_WINDOWEVENT_MINIMIZED:
                minimized_ = true;
                SendInputFocusEvent();
                break;

            case SDL_WINDOWEVENT_MAXIMIZED:
            case SDL_WINDOWEVENT_RESTORED:
                minimized_ = false;
                SendInputFocusEvent();
            #ifdef IOS
                // On iOS we never lose the GL context, but may have done GPU object changes that could not be applied yet.
                // Apply them now
                graphics_->Restore();
            #endif
                break;

            #ifdef ANDROID
            case SDL_WINDOWEVENT_FOCUS_GAINED:
                // Restore GPU objects to the new GL context
                graphics_->Restore();
                break;
            #endif

            case SDL_WINDOWEVENT_RESIZED:
                graphics_->WindowResized();
                break;
            case SDL_WINDOWEVENT_MOVED:
                graphics_->WindowMoved();
                break;
            }
        }
        break;

    case SDL_DROPFILE:
        {
            using namespace DropFile;

            VariantMap& eventData = GetEventDataMap();
            eventData[P_FILENAME] = GetInternalPath(String(evt.drop.file));
            SDL_free(evt.drop.file);

            SendEvent(E_DROPFILE, eventData);
        }
        break;

    case SDL_QUIT:
        SendEvent(E_EXITREQUESTED);
        break;
    }
}

void Input::HandleScreenMode(StringHash eventType, VariantMap& eventData)
{
    // Reset input state on subsequent initializations
    if (!initialized_)
        Initialize();
    else
        ResetState();

    // Re-enable cursor clipping, and re-center the cursor (if needed) to the new screen size, so that there is no erroneous
    // mouse move event. Also get new window ID if it changed
    SDL_Window* window = graphics_->GetImpl()->GetWindow();
    windowID_ = SDL_GetWindowID(window);

    if (!mouseVisible_)
    {
        IntVector2 center(graphics_->GetWidth() / 2, graphics_->GetHeight() / 2);
        SetMousePosition(center);
        lastMousePosition_ = center;
    }

    focusedThisFrame_ = true;

    // After setting a new screen mode we should not be minimized
    minimized_ = (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0;
}

void Input::HandleBeginFrame(StringHash eventType, VariantMap& eventData)
{
    // Update input right at the beginning of the frame
    Update();
}

void Input::HandleScreenJoystickTouch(StringHash eventType, VariantMap& eventData)
{
    using namespace TouchBegin;

    // Only interested in events from screen joystick(s)
    TouchState& state = touches_[eventData[P_TOUCHID].GetInt()];
    IntVector2 position(state.position_.x_, state.position_.y_);
    UIElement* element = eventType == E_TOUCHBEGIN ? GetSubsystem<UI>()->GetElementAt(position) : state.touchedElement_;
    if (!element)
        return;
    Variant variant = element->GetVar(VAR_SCREEN_JOYSTICK_ID);
    if (variant.IsEmpty())
        return;
    SDL_JoystickID joystickID = variant.GetInt();

    if (eventType == E_TOUCHEND)
        state.touchedElement_.Reset();
    else
        state.touchedElement_ = element;

    // Prepare a fake SDL event
    SDL_Event evt;

    const String& name = element->GetName();
    if (name.startsWith("Button"))
    {
        if (eventType == E_TOUCHMOVE)
            return;

        // Determine whether to inject a joystick event or keyboard/mouse event
        Variant keyBindingVar = element->GetVar(VAR_BUTTON_KEY_BINDING);
        Variant mouseButtonBindingVar = element->GetVar(VAR_BUTTON_MOUSE_BUTTON_BINDING);
        if (keyBindingVar.IsEmpty() && mouseButtonBindingVar.IsEmpty())
        {
            evt.type = eventType == E_TOUCHBEGIN ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
            evt.jbutton.which = joystickID;
            evt.jbutton.button = ToUInt(name.Substring(6));
        }
        else
        {
            if (!keyBindingVar.IsEmpty())
            {
                evt.type = eventType == E_TOUCHBEGIN ? SDL_KEYDOWN : SDL_KEYUP;
                evt.key.keysym.sym = keyBindingVar.GetInt();
                evt.key.keysym.scancode = SDL_SCANCODE_UNKNOWN;
            }
            if (!mouseButtonBindingVar.IsEmpty())
            {
                // Mouse button are sent as extra events besides key events
                // Disable touch emulation handling during this to prevent endless loop
                bool oldTouchEmulation = touchEmulation_;
                touchEmulation_ = false;

                SDL_Event evt;
                evt.type = eventType == E_TOUCHBEGIN ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
                evt.button.button = mouseButtonBindingVar.GetInt();
                HandleSDLEvent(&evt);

                touchEmulation_ = oldTouchEmulation;
            }
        }
    }
    else if (name.startsWith("Hat"))
    {
        Variant keyBindingVar = element->GetVar(VAR_BUTTON_KEY_BINDING);
        if (keyBindingVar.IsEmpty())
        {
            evt.type = SDL_JOYHATMOTION;
            evt.jaxis.which = joystickID;
            evt.jhat.hat = ToUInt(name.Substring(3));
            evt.jhat.value = HAT_CENTER;
            if (eventType != E_TOUCHEND)
            {
                IntVector2 relPosition = position - element->GetScreenPosition() - element->GetSize() / 2;
                if (relPosition.y_ < 0 && Abs(relPosition.x_ * 3 / 2) < Abs(relPosition.y_))
                    evt.jhat.value |= HAT_UP;
                if (relPosition.y_ > 0 && Abs(relPosition.x_ * 3 / 2) < Abs(relPosition.y_))
                    evt.jhat.value |= HAT_DOWN;
                if (relPosition.x_ < 0 && Abs(relPosition.y_ * 3 / 2) < Abs(relPosition.x_))
                    evt.jhat.value |= HAT_LEFT;
                if (relPosition.x_ > 0 && Abs(relPosition.y_ * 3 / 2) < Abs(relPosition.x_))
                    evt.jhat.value |= HAT_RIGHT;
            }
        }
        else
        {
            // Hat is binded by 4 keys, like 'WASD'
            String keyBinding = keyBindingVar.GetString();

            if (eventType == E_TOUCHEND)
            {
                evt.type = SDL_KEYUP;
                evt.key.keysym.sym = element->GetVar(VAR_LAST_KEYSYM).GetInt();
                if (!evt.key.keysym.sym)
                    return;

                element->SetVar(VAR_LAST_KEYSYM, 0);
            }
            else
            {
                evt.type = SDL_KEYDOWN;
                IntVector2 relPosition = position - element->GetScreenPosition() - element->GetSize() / 2;
                if (relPosition.y_ < 0 && Abs(relPosition.x_ * 3 / 2) < Abs(relPosition.y_))
                    evt.key.keysym.sym = keyBinding[0];
                else if (relPosition.y_ > 0 && Abs(relPosition.x_ * 3 / 2) < Abs(relPosition.y_))
                    evt.key.keysym.sym = keyBinding[1];
                else if (relPosition.x_ < 0 && Abs(relPosition.y_ * 3 / 2) < Abs(relPosition.x_))
                    evt.key.keysym.sym = keyBinding[2];
                else if (relPosition.x_ > 0 && Abs(relPosition.y_ * 3 / 2) < Abs(relPosition.x_))
                    evt.key.keysym.sym = keyBinding[3];
                else
                    return;

                if (eventType == E_TOUCHMOVE && evt.key.keysym.sym != element->GetVar(VAR_LAST_KEYSYM).GetInt())
                {
                    // Dragging past the directional boundary will cause an additional key up event for previous key symbol
                    SDL_Event evt;
                    evt.type = SDL_KEYUP;
                    evt.key.keysym.sym = element->GetVar(VAR_LAST_KEYSYM).GetInt();
                    if (evt.key.keysym.sym)
                    {
                        evt.key.keysym.scancode = SDL_SCANCODE_UNKNOWN;
                        HandleSDLEvent(&evt);
                    }

                    element->SetVar(VAR_LAST_KEYSYM, 0);
                }

                evt.key.keysym.scancode = SDL_SCANCODE_UNKNOWN;

                element->SetVar(VAR_LAST_KEYSYM, evt.key.keysym.sym);
            }
        }
    }
    else
        return;

    // Handle the fake SDL event to turn it into Urho3D genuine event
    HandleSDLEvent(&evt);
}

}
