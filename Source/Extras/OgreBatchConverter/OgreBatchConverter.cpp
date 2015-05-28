// OgreBatchConverter.cpp : Defines the entry point for the console application.
//

#include <Urho3D/Urho3D.h>
#include <Urho3D/Core/Context.h>
#include <Urho3D/IO/FileSystem.h>
#include <Urho3D/Core/ProcessUtils.h>

#include <stdio.h>

using namespace Urho3D;

SharedPtr<Context> context(new Context());
SharedPtr<FileSystem> fileSystem(new FileSystem(context));

int main(int argc, char** argv)
{
    // Take in account args and place on OgreImporter args
    QStringList args = ParseArguments(argc, argv);
    QStringList files;
    QString currentDir = fileSystem->GetCurrentDir();

    // Try to execute OgreImporter from same directory as this executable
    QString ogreImporterName = fileSystem->GetProgramDir() + "OgreImporter";

    printf("\n\nOgreBatchConverter requires OgreImporter.exe on same directory");
    printf("\nSearching Ogre file in Xml format in %s\n" ,qPrintable(currentDir));
    fileSystem->ScanDir(files, currentDir, "*.xml", SCAN_FILES, true);
    printf("\nFound %d files\n", files.size());
    #ifdef WIN32
    if (files.Size()) fileSystem->SystemCommand("pause");
    #endif

    for (unsigned i = 0 ; i < files.size(); i++)
    {
        QStringList cmdArgs;
        cmdArgs.push_back(files[i]);
        cmdArgs.push_back(ReplaceExtension(files[i], ".mdl"));
        cmdArgs += args;

        QString cmdPreview = ogreImporterName;
        for (unsigned j = 0; j < cmdArgs.size(); j++)
            cmdPreview += " " + cmdArgs[j];

        printf("\n%s", qPrintable(cmdPreview));
        fileSystem->SystemRun(ogreImporterName, cmdArgs);
    }

    printf("\nExit\n");
    #ifdef WIN32
    fileSystem->SystemCommand("pause");
    #endif

    return 0;
}

