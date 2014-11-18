// OgreBatchConverter.cpp : Defines the entry point for the console application.
//

#include "Context.h"
#include "FileSystem.h"
#include "ProcessUtils.h"

#include <stdio.h>

using namespace Urho3D;

SharedPtr<Context> context(new Context());
SharedPtr<FileSystem> fileSystem(new FileSystem(context));

int main(int argc, char** argv)
{
    // Take in account args and place on OgreImporter args
    const Vector<String>& args = ParseArguments(argc, argv);
    Vector<String> files;
    String currentDir = fileSystem->GetCurrentDir();

    // Try to execute OgreImporter from same directory as this executable
    String ogreImporterName = fileSystem->GetProgramDir() + "OgreImporter";

    printf("\n\nOgreBatchConverter requires OgreImporter.exe on same directory");
    printf("\nSearching Ogre file in Xml format in %s\n" ,currentDir.CString());
    fileSystem->ScanDir(files, currentDir, "*.xml", SCAN_FILES, true);
    printf("\nFound %d files\n", files.size());
    #ifdef WIN32
    if (files.Size()) fileSystem->SystemCommand("pause");
    #endif

    for (unsigned i = 0 ; i < files.size(); i++)
    {
        Vector<String> cmdArgs;
        cmdArgs.push_back(files[i]);
        cmdArgs.push_back(ReplaceExtension(files[i], ".mdl"));
        cmdArgs.insert(cmdArgs.end(),args.begin(),args.end());

        String cmdPreview = ogreImporterName;
        for (unsigned j = 0; j < cmdArgs.size(); j++)
            cmdPreview += " " + cmdArgs[j];

        printf("\n%s", cmdPreview.CString());
        fileSystem->SystemRun(ogreImporterName, cmdArgs);
    }

    printf("\nExit\n");
    #ifdef WIN32
    fileSystem->SystemCommand("pause");
    #endif

    return 0;
}

