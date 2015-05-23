//
// Copyright (c) 2008-2015 the Urho3D project.
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
#include "../Container/ArrayPtr.h"
#include "../Core/Context.h"
#include "../Core/CoreEvents.h"
#include "../Engine/EngineEvents.h"
#include "../IO/File.h"
#include "../IO/FileSystem.h"
#include "../IO/IOEvents.h"
#include "../IO/Log.h"
#include "../Core/Thread.h"

#include <SDL/SDL_filesystem.h>

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#ifdef WIN32
#ifndef _MSC_VER
#define _WIN32_IE 0x501
#endif
#include <windows.h>
#include <shellapi.h>
#include <direct.h>
#include <shlobj.h>
#include <sys/types.h>
#include <sys/utime.h>
#else
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <utime.h>
#include <sys/wait.h>
#define MAX_PATH 256
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#ifdef ANDROID
extern "C" const char* SDL_Android_GetFilesDir();
#endif
#ifdef IOS
extern "C" const char* SDL_IOS_GetResourceDir();
extern "C" const char* SDL_IOS_GetDocumentsDir();
#endif

#include "../DebugNew.h"

namespace Urho3D
{

int DoSystemCommand(const String& commandLine, bool redirectToLog, Context* context)
{
    if (!redirectToLog)
        return system(commandLine.CString());

    // Get a platform-agnostic temporary file name for stderr redirection
    String stderrFilename;
    String adjustedCommandLine(commandLine);
    char* prefPath = SDL_GetPrefPath("urho3d", "temp");
    if (prefPath)
    {
        stderrFilename = String(prefPath) + "command-stderr";
        adjustedCommandLine += " 2>" + stderrFilename;
        SDL_free(prefPath);
    }

    #ifdef _MSC_VER
    #define popen _popen
    #define pclose _pclose
    #endif

    // Use popen/pclose to capture the stdout and stderr of the command
    FILE *file = popen(adjustedCommandLine.CString(), "r");
    if (!file)
        return -1;

    // Capture the standard output stream
    char buffer[128];
    while (!feof(file))
    {
        if (fgets(buffer, sizeof(buffer), file))
            LOGRAW(String(buffer));
    }
    int exitCode = pclose(file);

    // Capture the standard error stream
    if (!stderrFilename.isEmpty())
    {
        SharedPtr<File> errFile(new File(context, stderrFilename, FILE_READ));
        while (!errFile->IsEof())
        {
            unsigned numRead = errFile->Read(buffer, sizeof(buffer));
            if (numRead)
                Log::WriteRaw(String(buffer, numRead), true);
        }
    }

    return exitCode;
}

int DoSystemRun(const String& fileName, const Vector<String>& arguments)
{
    String fixedFileName = GetNativePath(fileName);

    #ifdef WIN32
    // Add .exe extension if no extension defined
    if (GetExtension(fixedFileName).Empty())
        fixedFileName += ".exe";

    String commandLine = "\"" + fixedFileName + "\"";
    for (unsigned i = 0; i < arguments.Size(); ++i)
        commandLine += " " + arguments[i];

    STARTUPINFOW startupInfo;
    PROCESS_INFORMATION processInfo;
    memset(&startupInfo, 0, sizeof startupInfo);
    memset(&processInfo, 0, sizeof processInfo);

    WString commandLineW(commandLine);
    if (!CreateProcessW(NULL, (wchar_t*)commandLineW.CString(), 0, 0, 0, CREATE_NO_WINDOW, 0, 0, &startupInfo, &processInfo))
        return -1;

    WaitForSingleObject(processInfo.hProcess, INFINITE);
    DWORD exitCode;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);

    CloseHandle(processInfo.hProcess);
    CloseHandle(processInfo.hThread);

    return exitCode;
    #else
    pid_t pid = fork();
    if (!pid)
    {
        PODVector<const char*> argPtrs;
        argPtrs.push_back(fixedFileName.CString());
        for (unsigned i = 0; i < arguments.size(); ++i)
            argPtrs.push_back(arguments[i].CString());
        argPtrs.push_back(nullptr);

        execvp(argPtrs[0], (char**)&argPtrs[0]);
        return -1; // Return -1 if we could not spawn the process
    }
    else if (pid > 0)
    {
        int exitCode;
        wait(&exitCode);
        return exitCode;
    }
    else
        return -1;
    #endif
}

/// Base class for async execution requests.
class AsyncExecRequest : public Thread
{
public:
    /// Construct.
    AsyncExecRequest(unsigned& requestID) :
        requestID_(requestID),
        completed_(false)
    {
        // Increment ID for next request
        ++requestID;
        if (requestID == M_MAX_UNSIGNED)
            requestID = 1;
    }

    /// Return request ID.
    unsigned GetRequestID() const { return requestID_; }
    /// Return exit code. Valid when IsCompleted() is true.
    int GetExitCode() const { return exitCode_; }
    /// Return completion status.
    bool IsCompleted() const { return completed_; }

protected:
    /// Request ID.
    unsigned requestID_;
    /// Exit code.
    int exitCode_;
    /// Completed flag.
    volatile bool completed_;
};

/// Async system command operation.
class AsyncSystemCommand : public AsyncExecRequest
{
public:
    /// Construct and run.
    AsyncSystemCommand(unsigned requestID, const String& commandLine) :
        AsyncExecRequest(requestID),
        commandLine_(commandLine)
    {
        Run();
    }

    /// The function to run in the thread.
    virtual void ThreadFunction() override
    {
        exitCode_ = DoSystemCommand(commandLine_, false, nullptr);
        completed_ = true;
    }

private:
    /// Command line.
    String commandLine_;
};

/// Async system run operation.
class AsyncSystemRun : public AsyncExecRequest
{
public:
    /// Construct and run.
    AsyncSystemRun(unsigned requestID, const String& fileName, const Vector<String>& arguments) :
        AsyncExecRequest(requestID),
        fileName_(fileName),
        arguments_(arguments)
    {
        Run();
    }

    /// The function to run in the thread.
    virtual void ThreadFunction() override
    {
        exitCode_ = DoSystemRun(fileName_, arguments_);
        completed_ = true;
    }

private:
    /// File to run.
    String fileName_;
    /// Command line split in arguments.
    const Vector<String>& arguments_;
};

FileSystem::FileSystem(Context* context) :
    Object(context),
    nextAsyncExecID_(1),
    executeConsoleCommands_(false)
{
    SubscribeToEvent(E_BEGINFRAME, HANDLER(FileSystem, HandleBeginFrame));

    // Subscribe to console commands
    SetExecuteConsoleCommands(true);
}

FileSystem::~FileSystem()
{
    // If any async exec items pending, delete them
    if (asyncExecQueue_.size())
    {
        for (auto & elem : asyncExecQueue_)
            delete(elem);

        asyncExecQueue_.clear();
    }
}

bool FileSystem::SetCurrentDir(const String& pathName)
{
    if (!CheckAccess(pathName))
    {
        LOGERROR("Access denied to " + pathName);
        return false;
    }
    #ifdef WIN32
    if (SetCurrentDirectoryW(GetWideNativePath(pathName).CString()) == FALSE)
    {
        LOGERROR("Failed to change directory to " + pathName);
        return false;
    }
    #else
    if (chdir(GetNativePath(pathName).CString()) != 0)
    {
        LOGERROR("Failed to change directory to " + pathName);
        return false;
    }
    #endif

    return true;
}

bool FileSystem::CreateDir(const String& pathName)
{
    if (!CheckAccess(pathName))
    {
        LOGERROR("Access denied to " + pathName);
        return false;
    }

    #ifdef WIN32
    bool success = (CreateDirectoryW(GetWideNativePath(RemoveTrailingSlash(pathName)).CString(), 0) == TRUE) ||
        (GetLastError() == ERROR_ALREADY_EXISTS);
    #else
    bool success = mkdir(GetNativePath(RemoveTrailingSlash(pathName)).CString(), S_IRWXU) == 0 || errno == EEXIST;
    #endif

    if (success)
        LOGDEBUG("Created directory " + pathName);
    else
        LOGERROR("Failed to create directory " + pathName);

    return success;
}

void FileSystem::SetExecuteConsoleCommands(bool enable)
{
    if (enable == executeConsoleCommands_)
        return;

    executeConsoleCommands_ = enable;
    if (enable)
        SubscribeToEvent(E_CONSOLECOMMAND, HANDLER(FileSystem, HandleConsoleCommand));
    else
        UnsubscribeFromEvent(E_CONSOLECOMMAND);
}

int FileSystem::SystemCommand(const String& commandLine, bool redirectStdOutToLog)
{
    if (allowedPaths_.isEmpty())
        return DoSystemCommand(commandLine, redirectStdOutToLog, context_);
    else
    {
        LOGERROR("Executing an external command is not allowed");
        return -1;
    }
}

int FileSystem::SystemRun(const String& fileName, const Vector<String>& arguments)
{
    if (allowedPaths_.isEmpty())
        return DoSystemRun(fileName, arguments);
    else
    {
        LOGERROR("Executing an external command is not allowed");
        return -1;
    }
}

unsigned FileSystem::SystemCommandAsync(const String& commandLine)
{
    if (allowedPaths_.isEmpty())
    {
        unsigned requestID = nextAsyncExecID_;
        AsyncSystemCommand* cmd = new AsyncSystemCommand(nextAsyncExecID_, commandLine);
        asyncExecQueue_.push_back(cmd);
        return requestID;
    }
    else
    {
        LOGERROR("Executing an external command is not allowed");
        return M_MAX_UNSIGNED;
    }
}

unsigned FileSystem::SystemRunAsync(const String& fileName, const Vector<String>& arguments)
{
    if (allowedPaths_.isEmpty())
    {
        unsigned requestID = nextAsyncExecID_;
        AsyncSystemRun* cmd = new AsyncSystemRun(nextAsyncExecID_, fileName, arguments);
        asyncExecQueue_.push_back(cmd);
        return requestID;
    }
    else
    {
        LOGERROR("Executing an external command is not allowed");
        return M_MAX_UNSIGNED;
    }
}

bool FileSystem::SystemOpen(const String& fileName, const String& mode)
{
    if (allowedPaths_.isEmpty())
    {
        if (!FileExists(fileName) && !DirExists(fileName))
        {
            LOGERROR("File or directory " + fileName + " not found");
            return false;
        }

        #ifdef WIN32
        bool success = (size_t)ShellExecuteW(0, !mode.Empty() ? WString(mode).CString() : 0,
            GetWideNativePath(fileName).CString(), 0, 0, SW_SHOW) > 32;
        #else
        Vector<String> arguments;
        arguments.push_back(fileName);
        bool success = SystemRun(
        #if defined(__APPLE__)
                "/usr/bin/open",
        #else
                "/usr/bin/xdg-open",
        #endif
                arguments) == 0;
        #endif
        if (!success)
            LOGERROR("Failed to open " + fileName + " externally");
        return success;
    }
    else
    {
        LOGERROR("Opening a file externally is not allowed");
        return false;
    }
}

bool FileSystem::Copy(const String& srcFileName, const String& destFileName)
{
    if (!CheckAccess(GetPath(srcFileName)))
    {
        LOGERROR("Access denied to " + srcFileName);
        return false;
    }
    if (!CheckAccess(GetPath(destFileName)))
    {
        LOGERROR("Access denied to " + destFileName);
        return false;
    }

    SharedPtr<File> srcFile(new File(context_, srcFileName, FILE_READ));
    if (!srcFile->IsOpen())
        return false;
    SharedPtr<File> destFile(new File(context_, destFileName, FILE_WRITE));
    if (!destFile->IsOpen())
        return false;

    unsigned fileSize = srcFile->GetSize();
    SharedArrayPtr<unsigned char> buffer(new unsigned char[fileSize]);

    unsigned bytesRead = srcFile->Read(buffer.Get(), fileSize);
    unsigned bytesWritten = destFile->Write(buffer.Get(), fileSize);
    return bytesRead == fileSize && bytesWritten == fileSize;
}

bool FileSystem::Rename(const String& srcFileName, const String& destFileName)
{
    if (!CheckAccess(GetPath(srcFileName)))
    {
        LOGERROR("Access denied to " + srcFileName);
        return false;
    }
    if (!CheckAccess(GetPath(destFileName)))
    {
        LOGERROR("Access denied to " + destFileName);
        return false;
    }

    #ifdef WIN32
    return MoveFileW(GetWideNativePath(srcFileName).CString(), GetWideNativePath(destFileName).CString()) != 0;
    #else
    return rename(GetNativePath(srcFileName).CString(), GetNativePath(destFileName).CString()) == 0;
    #endif
}

bool FileSystem::Delete(const String& fileName)
{
    if (!CheckAccess(GetPath(fileName)))
    {
        LOGERROR("Access denied to " + fileName);
        return false;
    }

    #ifdef WIN32
    return DeleteFileW(GetWideNativePath(fileName).CString()) != 0;
    #else
    return remove(GetNativePath(fileName).CString()) == 0;
    #endif
}

String FileSystem::GetCurrentDir() const
{
    #ifdef WIN32
    wchar_t path[MAX_PATH];
    path[0] = 0;
    GetCurrentDirectoryW(MAX_PATH, path);
    return AddTrailingSlash(String(path));
    #else
    char path[MAX_PATH];
    path[0] = 0;
    getcwd(path, MAX_PATH);
    return AddTrailingSlash(String(path));
    #endif
}

bool FileSystem::CheckAccess(const String& pathName) const
{
    String fixedPath = AddTrailingSlash(pathName);

    // If no allowed directories defined, succeed always
    if (allowedPaths_.isEmpty())
        return true;

    // If there is any attempt to go to a parent directory, disallow
    if (fixedPath.contains(".."))
        return false;

    // Check if the path is a partial match of any of the allowed directories
    for (const String &i : allowedPaths_)
    {
        if (fixedPath.indexOf(i) == 0)
            return true;
    }

    // Not found, so disallow
    return false;
}

unsigned FileSystem::GetLastModifiedTime(const String& fileName) const
{
    if (fileName.isEmpty() || !CheckAccess(fileName))
        return 0;

    #ifdef WIN32
    struct _stat st;
    if (!_stat(fileName.CString(), &st))
        return (unsigned)st.st_mtime;
    else
        return 0;
    #else
    struct stat st;
    if (!stat(fileName.CString(), &st))
        return (unsigned)st.st_mtime;
    else
        return 0;
    #endif
}

bool FileSystem::FileExists(const String& fileName) const
{
    if (!CheckAccess(GetPath(fileName)))
        return false;

    String fixedName = GetNativePath(RemoveTrailingSlash(fileName));

    #ifdef ANDROID
    if (fixedName.StartsWith("/apk/"))
    {
        SDL_RWops* rwOps = SDL_RWFromFile(fileName.Substring(5).CString(), "rb");
        if (rwOps)
        {
            SDL_RWclose(rwOps);
            return true;
        }
        else
            return false;
    }
    #endif

    #ifdef WIN32
    DWORD attributes = GetFileAttributesW(WString(fixedName).CString());
    if (attributes == INVALID_FILE_ATTRIBUTES || attributes & FILE_ATTRIBUTE_DIRECTORY)
        return false;
    #else
    struct stat st;
    if (stat(fixedName.CString(), &st) || st.st_mode & S_IFDIR)
        return false;
    #endif

    return true;
}

bool FileSystem::DirExists(const String& pathName) const
{
    if (!CheckAccess(pathName))
        return false;

    #ifndef WIN32
    // Always return true for the root directory
    if (pathName == "/")
        return true;
    #endif

    String fixedName = GetNativePath(RemoveTrailingSlash(pathName));

    #ifdef ANDROID
    /// \todo Actually check for existence, now true is always returned for directories within the APK
    if (fixedName.StartsWith("/apk/"))
        return true;
    #endif

    #ifdef WIN32
    DWORD attributes = GetFileAttributesW(WString(fixedName).CString());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    #else
    struct stat st;
    if (stat(fixedName.CString(), &st) || !(st.st_mode & S_IFDIR))
        return false;
    #endif

    return true;
}

void FileSystem::ScanDir(Vector<String>& result, const String& pathName, const String& filter, unsigned flags, bool recursive) const
{
    result.clear();

    if (CheckAccess(pathName))
    {
        String initialPath = AddTrailingSlash(pathName);
        ScanDirInternal(result, initialPath, initialPath, filter, flags, recursive);
    }
}

String FileSystem::GetProgramDir() const
{
    // Return cached value if possible
    if (!programDir_.isEmpty())
        return programDir_;

    #if defined(ANDROID)
    // This is an internal directory specifier pointing to the assets in the .apk
    // Files from this directory will be opened using special handling
    programDir_ = "/apk/";
    return programDir_;
    #elif defined(IOS)
    programDir_ = AddTrailingSlash(SDL_IOS_GetResourceDir());
    return programDir_;
    #elif defined(WIN32)
    wchar_t exeName[MAX_PATH];
    exeName[0] = 0;
    GetModuleFileNameW(0, exeName, MAX_PATH);
    programDir_ = GetPath(String(exeName));
    #elif defined(__APPLE__)
    char exeName[MAX_PATH];
    memset(exeName, 0, MAX_PATH);
    unsigned size = MAX_PATH;
    _NSGetExecutablePath(exeName, &size);
    programDir_ = GetPath(String(exeName));
    #elif defined(__linux__)
    char exeName[MAX_PATH];
    memset(exeName, 0, MAX_PATH);
    pid_t pid = getpid();
    String link = "/proc/" + String(pid) + "/exe";
    readlink(link.CString(), exeName, MAX_PATH);
    programDir_ = GetPath(String(exeName));
    #endif

    // If the executable directory does not contain CoreData & Data directories, but the current working directory does, use the
    // current working directory instead
    /// \todo Should not rely on such fixed convention
    String currentDir = GetCurrentDir();
    if (!DirExists(programDir_ + "CoreData") && !DirExists(programDir_ + "Data") && (DirExists(currentDir + "CoreData") ||
        DirExists(currentDir + "Data")))
        programDir_ = currentDir;

    // Sanitate /./ construct away
    programDir_.replace("/./", "/");

    return programDir_;
}

String FileSystem::GetUserDocumentsDir() const
{
    #if defined(ANDROID)
    return AddTrailingSlash(SDL_Android_GetFilesDir());
    #elif defined(IOS)
    return AddTrailingSlash(SDL_IOS_GetDocumentsDir());
    #elif defined(WIN32)
    wchar_t pathName[MAX_PATH];
    pathName[0] = 0;
    SHGetSpecialFolderPathW(0, pathName, CSIDL_PERSONAL, 0);
    return AddTrailingSlash(String(pathName));
    #else
    char pathName[MAX_PATH];
    pathName[0] = 0;
    strcpy(pathName, getenv("HOME"));
    return AddTrailingSlash(String(pathName));
    #endif
}

String FileSystem::GetAppPreferencesDir(const String& org, const String& app) const
{
    String dir;
    char* prefPath = SDL_GetPrefPath(org.CString(), app.CString());
    if (prefPath)
    {
        dir = GetInternalPath(String(prefPath));
        SDL_free(prefPath);
    }
    else
        LOGWARNING("Could not get application preferences directory");

    return dir;
}

void FileSystem::RegisterPath(const String& pathName)
{
    if (pathName.isEmpty())
        return;

    allowedPaths_.insert(AddTrailingSlash(pathName));
}

bool FileSystem::SetLastModifiedTime(const String& fileName, unsigned newTime)
{
    if (fileName.isEmpty() || !CheckAccess(fileName))
        return false;

    #ifdef WIN32
    struct _stat oldTime;
    struct _utimbuf newTimes;
    if (_stat(fileName.CString(), &oldTime) != 0)
        return false;
    newTimes.actime = oldTime.st_atime;
    newTimes.modtime = newTime;
    return _utime(fileName.CString(), &newTimes) == 0;
    #else
    struct stat oldTime;
    struct utimbuf newTimes;
    if (stat(fileName.CString(), &oldTime) != 0)
        return false;
    newTimes.actime = oldTime.st_atime;
    newTimes.modtime = newTime;
    return utime(fileName.CString(), &newTimes) == 0;
    #endif
}

void FileSystem::ScanDirInternal(Vector<String>& result, String path, const String& startPath,
    const String& filter, unsigned flags, bool recursive) const
{
    path = AddTrailingSlash(path);
    String deltaPath;
    if (path.length() > startPath.length())
        deltaPath = path.Substring(startPath.length());

    String filterExtension = filter.Substring(filter.indexOf('.'));
    if (filterExtension.contains('*'))
        filterExtension.clear();

    #ifdef WIN32
    WIN32_FIND_DATAW info;
    HANDLE handle = FindFirstFileW(WString(path + "*").CString(), &info);
    if (handle != INVALID_HANDLE_VALUE)
    {
        do
        {
            String fileName(info.cFileName);
            if (!fileName.Empty())
            {
                if (info.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN && !(flags & SCAN_HIDDEN))
                    continue;
                if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    if (flags & SCAN_DIRS)
                        result.Push(deltaPath + fileName);
                    if (recursive && fileName != "." && fileName != "..")
                        ScanDirInternal(result, path + fileName, startPath, filter, flags, recursive);
                }
                else if (flags & SCAN_FILES)
                {
                    if (filterExtension.Empty() || fileName.EndsWith(filterExtension))
                        result.Push(deltaPath + fileName);
                }
            }
        }
        while (FindNextFileW(handle, &info));

        FindClose(handle);
    }
    #else
    DIR *dir;
    struct dirent *de;
    struct stat st;
    dir = opendir(GetNativePath(path).CString());
    if (dir)
    {
        while ((de = readdir(dir)))
        {
            /// \todo Filename may be unnormalized Unicode on Mac OS X. Re-normalize as necessary
            String fileName(de->d_name);
            bool normalEntry = fileName != "." && fileName != "..";
            if (normalEntry && !(flags & SCAN_HIDDEN) && fileName.startsWith("."))
                continue;
            String pathAndName = path + fileName;
            if (!stat(pathAndName.CString(), &st))
            {
                if (st.st_mode & S_IFDIR)
                {
                    if (flags & SCAN_DIRS)
                        result.push_back(deltaPath + fileName);
                    if (recursive && normalEntry)
                        ScanDirInternal(result, path + fileName, startPath, filter, flags, recursive);
                }
                else if (flags & SCAN_FILES)
                {
                    if (filterExtension.isEmpty() || fileName.endsWith(filterExtension))
                        result.push_back(deltaPath + fileName);
                }
            }
        }
        closedir(dir);
    }
    #endif
}

void FileSystem::HandleBeginFrame(StringHash eventType, VariantMap& eventData)
{
    /// Go through the execution queue and post + remove completed requests
    for (List<AsyncExecRequest*>::iterator i = asyncExecQueue_.begin(); i != asyncExecQueue_.end();)
    {
        AsyncExecRequest* request = *i;
        if (request->IsCompleted())
        {
            using namespace AsyncExecFinished;

            VariantMap& eventData = GetEventDataMap();
            eventData[P_REQUESTID] = request->GetRequestID();
            eventData[P_EXITCODE] = request->GetExitCode();
            SendEvent(E_ASYNCEXECFINISHED, eventData);

            delete request;
            i = asyncExecQueue_.erase(i);
        }
        else
            ++i;
    }
}

void FileSystem::HandleConsoleCommand(StringHash eventType, VariantMap& eventData)
{
    using namespace ConsoleCommand;
    if (eventData[P_ID].GetString() == GetTypeName())
        SystemCommand(eventData[P_COMMAND].GetString(), true);
}

void SplitPath(const String& fullPath, String& pathName, String& fileName, String& extension, bool lowercaseExtension)
{
    String fullPathCopy = GetInternalPath(fullPath);

    unsigned extPos = fullPathCopy.lastIndexOf('.');
    unsigned pathPos = fullPathCopy.lastIndexOf('/');

    if (extPos != String::NPOS && (pathPos == String::NPOS || extPos > pathPos))
    {
        extension = fullPathCopy.Substring(extPos);
        if (lowercaseExtension)
            extension = extension.toLower();
        fullPathCopy = fullPathCopy.Substring(0, extPos);
    }
    else
        extension.clear();

    pathPos = fullPathCopy.lastIndexOf('/');
    if (pathPos != String::NPOS)
    {
        fileName = fullPathCopy.Substring(pathPos + 1);
        pathName = fullPathCopy.Substring(0, pathPos + 1);
    }
    else
    {
        fileName = fullPathCopy;
        pathName.clear();
    }
}

String GetPath(const String& fullPath)
{
    String path, file, extension;
    SplitPath(fullPath, path, file, extension);
    return path;
}

String GetFileName(const String& fullPath)
{
    String path, file, extension;
    SplitPath(fullPath, path, file, extension);
    return file;
}

String GetExtension(const String& fullPath, bool lowercaseExtension)
{
    String path, file, extension;
    SplitPath(fullPath, path, file, extension, lowercaseExtension);
    return extension;
}

String GetFileNameAndExtension(const String& fileName, bool lowercaseExtension)
{
    String path, file, extension;
    SplitPath(fileName, path, file, extension, lowercaseExtension);
    return file + extension;
}

String ReplaceExtension(const String& fullPath, const String& newExtension)
{
    String path, file, extension;
    SplitPath(fullPath, path, file, extension);
    return path + file + newExtension;
}

String AddTrailingSlash(const String& pathName)
{
    String ret = pathName.trimmed();
    ret.replace('\\', '/');
    if (!ret.isEmpty() && ret.Back() != '/')
        ret += '/';
    return ret;
}

String RemoveTrailingSlash(const String& pathName)
{
    String ret = pathName.trimmed();
    ret.replace('\\', '/');
    if (!ret.isEmpty() && ret.Back() == '/')
        ret.resize(ret.length() - 1);
    return ret;
}

String GetParentPath(const String& path)
{
    unsigned pos = RemoveTrailingSlash(path).lastIndexOf('/');
    if (pos != String::NPOS)
        return path.Substring(0, pos + 1);
    else
        return String();
}

String GetInternalPath(const String& pathName)
{
    return pathName.replaced('\\', '/');
}

String GetNativePath(const String& pathName)
{
#ifdef WIN32
    return pathName.replaced('/', '\\');
#else
    return pathName;
#endif
}

WString GetWideNativePath(const String& pathName)
{
#ifdef WIN32
    return WString(pathName.replaced('/', '\\'));
#else
    return WString(pathName);
#endif
}

bool IsAbsolutePath(const String& pathName)
{
    if (pathName.isEmpty())
        return false;

    String path = GetInternalPath(pathName);

    if (path[0] == '/')
        return true;

#ifdef WIN32
    if (path.Length() > 1 && IsAlpha(path[0]) && path[1] == ':')
        return true;
#endif

    return false;
}

}
