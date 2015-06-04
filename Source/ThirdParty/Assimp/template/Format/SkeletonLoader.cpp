/*
---------------------------------------------------------------------------
Open Asset Import Library (assimp)
---------------------------------------------------------------------------

Copyright (c) 2006-2012, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the following
conditions are met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.

* Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the
  following disclaimer in the documentation and/or other
  materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
  contributors may be used to endorse or promote products
  derived from this software without specific prior
  written permission of the assimp team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
---------------------------------------------------------------------------
*/

/** @file  SkeletonLoader.cpp
 *  @brief Implementation of the DUMMY importer class
 */

// internal headers
#include "SkeletonLoader.h"
#include "ParsingUtils.h"
#include "fast_atof.h"
#include "../include/assimp/IOSystem.hpp"
#include "../include/assimp/scene.h"
#include "../include/assimp/DefaultLogger.hpp"

#include <memory>

using namespace Assimp;

// Required to define a Registration class, and init function
#include "ImporterRegistry.h"
DEFINE_FORMAT(SkeletonImporter,DUMMY)

// TODO: define proper aiImporterDesc values here
static const aiImporterDesc desc = {
    "Short name of the Importer",
    "",
    "",
    "",
    0,
    0,
    0,
    0,
    0,
    "off"
};

// ------------------------------------------------------------------------------------------------
// Constructor to be privately used by Importer
SkeletonImporter::SkeletonImporter()
{}

// ------------------------------------------------------------------------------------------------
// Destructor, private as well
SkeletonImporter::~SkeletonImporter()
{}

// ------------------------------------------------------------------------------------------------
// Returns whether the class can handle the format of the given file.
bool SkeletonImporter::CanRead( const std::string& pFile, IOSystem* pIOHandler, bool checkSig) const
{
    const std::string extension = GetExtension(pFile);

    if (extension == "DUMMY")
        return true;
    else if (!extension.length() || checkSig)
    {
        if (!pIOHandler)return true;
        const char* tokens[] = {"DUMMY"};
        return SearchFileHeaderForToken(pIOHandler,pFile,tokens,1);
    }
    return false;
}

// ------------------------------------------------------------------------------------------------
const aiImporterDesc* SkeletonImporter::GetInfo () const
{
    return &desc;
}

// ------------------------------------------------------------------------------------------------
// Imports the given file into the given scene structure.
void SkeletonImporter::InternReadFile( const std::string& pFile,
    aiScene* pScene, IOSystem* pIOHandler)
{
    std::unique_ptr<IOStream> file( pIOHandler->Open( pFile, "rb"));

    // Check whether we can read from the file
    if( file.get() == NULL) {
        throw DeadlyImportError( "Failed to open DUMMY file " + pFile + ".");
    }

    // allocate storage and copy the contents of the file to a memory buffer
    // First find out how many vertices we'll need
    // allocate storage for the output vertices
    // second: now parse all face indices
    // generate the output node graph
    pScene->mRootNode = new aiNode();
    pScene->mRootNode->mName.Set("<DUMMYRoot>");
    pScene->mRootNode->mMeshes = new unsigned int [pScene->mRootNode->mNumMeshes = 1];
    pScene->mRootNode->mMeshes[0] = 0;

    // generate a default material
    pScene->mMaterials = new aiMaterial*[pScene->mNumMaterials = 1];
    aiMaterial* pcMat = new aiMaterial();

    aiColor4D clr(0.6f,0.6f,0.6f,1.0f);
    pcMat->AddProperty(&clr,1,AI_MATKEY_COLOR_DIFFUSE);
    pScene->mMaterials[0] = pcMat;

    const int twosided =1;
    pcMat->AddProperty(&twosided,1,AI_MATKEY_TWOSIDED);
}
