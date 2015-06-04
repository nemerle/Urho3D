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
#include "DefaultIOSystem.h" // for completeBaseName
#include "ParsingUtils.h"
#include "fast_atof.h"
#include "../include/assimp/IOSystem.hpp"
#include "StreamReader.h"
#include "../include/assimp/scene.h"
#include "../include/assimp/vector2.h"
#include "../include/assimp/color4.h"
#include "../include/assimp/matrix4x4.h"
#include "../include/assimp/matrix3x3.h"
#include "../include/assimp/material.h"
#include "../include/assimp/DefaultLogger.hpp"

#include "half.h"
#include "normal.h"

#include <memory>
#include <sstream>
using namespace Assimp;

// Required to define a Registration class, and init function
#include "ImporterRegistry.h"
DEFINE_FORMAT(SkeletonImporter,EL3D)

// TODO: define proper aiImporterDesc values here
static const aiImporterDesc desc = {
    "Eternal Lands 3d asset importer",
    "nemerle",
    "nemerle",
    "",
    aiImporterFlags_SupportBinaryFlavour,
    1, 0,
    1, 1,
    "e3d"
};
namespace {
struct E3D_StreamInfo {
    int num_elements;
    int size;
    int offset;
};
void readStreamInfo(StreamReaderLE &src,E3D_StreamInfo &tgt) {
    tgt.num_elements = src.GetI4();
    tgt.size = src.GetI4();
    tgt.offset = src.GetI4();
}

struct VertexConfig
{
    uint8_t vertex_options;
    uint8_t vertex_format;
    uint32_t has_normal() const
    {
        return (vertex_options & 0x01) != 0;
    }

    uint32_t has_tangent() const
    {
        return (vertex_options & 0x02) != 0;
    }

    uint32_t has_secondary_texture_coordinate() const
    {
        return (vertex_options & 0x04) != 0;
    }

    uint32_t has_color() const
    {
        return (vertex_options & 0x08) != 0;
    }

    uint32_t half_position() const
    {
        return (vertex_format & 0x01) != 0;
    }

    uint32_t half_uv() const
    {
        return (vertex_format & 0x02) != 0;
    }

    uint32_t half_extra_uv() const
    {
        return (vertex_format & 0x04) != 0;
    }

    uint32_t compressed_normal() const
    {
        return (vertex_format & 0x08) != 0;
    }

    uint32_t short_index() const
    {
        return (vertex_format & 0x10) != 0;
    }

public:
    uint32_t check_vertex_size() const;

    uint32_t get_material_size() const;
};

struct e3d_material
{
   int options;
   char material_name[128];
   // bounding box
   aiVector3D min_vals;
   aiVector3D max_vals;

   int triangles_min_index;
   int triangles_max_index;
   int index;		/*!< index of the index list */
   int count;		/*!< number of indicies */
   void read(StreamReaderLE &src) {
        options = src.GetI4(); // !0 => has transparency
        src.CopyAndAdvance(material_name,128);
        src.CopyAndAdvance(&min_vals,sizeof(aiVector3D));
        src.CopyAndAdvance(&max_vals,sizeof(aiVector3D));
        triangles_min_index = src.GetI4();
        triangles_max_index = src.GetI4();
        index = src.GetI4();
        count = src.GetI4();
   }
};
struct e3d_extra_texture
{
    char material_name[128];	/*!< name of the material */
};
uint32_t VertexConfig::get_material_size() const
{
    uint32_t mat_size = sizeof(e3d_material);
    if (has_secondary_texture_coordinate())
        mat_size += sizeof(e3d_extra_texture);

    return mat_size;
}
uint32_t VertexConfig::check_vertex_size() const
{
    uint32_t tmp;

    tmp = 0;

    if (half_position())
        tmp += 3 * sizeof(uint16_t);
    else
        tmp += 3 * sizeof(float);

    if (half_uv())
        tmp += 2 * sizeof(uint16_t);
    else
        tmp += 2 * sizeof(float);

    if (has_normal())
    {
        if (compressed_normal())
            tmp += sizeof(uint16_t);
        else
            tmp += 3 * sizeof(float);
    }

    if (has_tangent())
    {
        if (compressed_normal())
            tmp += sizeof(uint16_t);
        else
            tmp += 3 * sizeof(float);
    }

    if (has_secondary_texture_coordinate())
    {
        if (half_extra_uv())
        {
            tmp += 2 * sizeof(uint16_t);
        }
        else
        {
            tmp += 2 * sizeof(float);
        }
    }

    if (has_color())
    {
        tmp += 4 * sizeof(uint8_t);
    }

    return tmp;
}
}
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
    uint8_t token[4] = {'e', '3', 'd', 'x'};
    std::string extension = GetExtension(pFile);
    if(extension == "e3d" ) {
        return CheckMagicToken(pIOHandler,pFile,token,1,0,4);
    }
    if (!extension.length() || checkSig) {
        return CheckMagicToken(pIOHandler,pFile,token,1,0,4);
    }
    return false;
}

// ------------------------------------------------------------------------------------------------
const aiImporterDesc* SkeletonImporter::GetInfo () const
{
    return &desc;
}
void dumpStreamInfo(const char *name,E3D_StreamInfo &src) {
    std::ostringstream ostr;
    ostr << "E3d file "<<name<<" count "<<src.num_elements<<" and size "<<src.size<<".";
    DefaultLogger::get()->warn(ostr.str());
}
struct E3DObject {
    std::vector<aiVector3D> positions;
    std::vector<aiVector2D> uv;
    std::vector<aiVector3D> normals;
    std::vector<aiColor4D> colors;
    std::vector<unsigned int> indices;
};
void createFacesCopyVertices(E3DObject &src,aiMesh *tgt,const std::vector<unsigned int> &indices) {
    tgt->mFaces = new aiFace[indices.size()/3];
    // now collect the vertex data of all data streams present in the imported mesh
    unsigned int newIndex = 0;
    tgt->mVertices = new aiVector3D[indices.size()];
    tgt->mNumVertices = indices.size();
    tgt->mNumFaces = indices.size()/3;
    if( !src.normals.empty() )
        tgt->mNormals = new aiVector3D[indices.size()];
    if( !src.colors.empty() )
        tgt->mColors[0] = new aiColor4D[indices.size()];
    tgt->mTextureCoords[0] = new aiVector3D[indices.size()];
    for( unsigned int c = 0; c < indices.size()/3; c++)
    {
        uint32_t idx[] = {
            indices[3*c],
            indices[3*c + 1],
            indices[3*c + 2]};

        // create face. either triangle or triangle fan depending on the index count
        aiFace& df = tgt->mFaces[c]; // destination face
        df.mNumIndices = 3;
        df.mIndices = new unsigned int[3];

        // collect vertex data for indices of this face
        for( unsigned int d = 0; d < 3; d++)
        {
            int src_idx = idx[d];
            df.mIndices[d] = newIndex;
            tgt->mTextureCoords[0][newIndex] = aiVector3D(src.uv[src_idx].x,1-src.uv[src_idx].y,0);
            tgt->mVertices[newIndex] = src.positions[src_idx];
            if(!src.normals.empty())
                tgt->mNormals[newIndex] = src.normals[src_idx];
            if(!src.colors.empty())
                tgt->mColors[0][newIndex] = src.colors[src_idx];
            newIndex++;
        }

    }
}
void generateMaterialsSplitMeshes(aiScene* pScene,StreamReaderLE &e3d_reader, E3D_StreamInfo &material_info,
                       E3DObject &src_mesh) {

    const int twosided =1;
    e3d_reader.SetCurrentPos(material_info.offset);

    pScene->mNumMaterials = material_info.num_elements;
    pScene->mNumMeshes = pScene->mNumMaterials;
    pScene->mMaterials = new aiMaterial*[pScene->mNumMaterials];
    pScene->mMeshes = new aiMesh*[pScene->mNumMaterials];
    pScene->mRootNode->mMeshes = new unsigned int [pScene->mNumMaterials];
    pScene->mRootNode->mNumMeshes = pScene->mNumMaterials;

    int file_pos;
    aiString tex_name;

    for (int i = 0; i < material_info.num_elements; i++)
    {
        e3d_material material;
        file_pos = e3d_reader.GetCurrentPos();
        material.read(e3d_reader);
        aiMaterial *mat = new aiMaterial;
        tex_name.Set(material.material_name);
        mat->AddProperty(&tex_name,AI_MATKEY_TEXTURE_DIFFUSE(0));
        if(material.options) {
            mat->AddProperty(&twosided,1,AI_MATKEY_TWOSIDED);
        }
        aiColor4D clr(1.0f,1.0f,1.0f,1.0f);
        mat->AddProperty(&clr,1,AI_MATKEY_COLOR_DIFFUSE);

        pScene->mMaterials[i] = mat;

        aiMesh *m = new aiMesh;
        std::vector<unsigned int> indices;
        for(int i=0; i<material.count; ++i) {
            indices.push_back(src_mesh.indices[material.index+i]);
        }
        createFacesCopyVertices(src_mesh,m,indices);
        file_pos += (material_info.size);
        e3d_reader.SetCurrentPos(file_pos);
        m->mMaterialIndex = i;
        pScene->mMeshes[i] = m;
        pScene->mRootNode->mMeshes[i] = i;
    }
}

void collectIndices(StreamReaderLE &e3d_reader, E3DObject &src, E3D_StreamInfo &index_info)
{
    e3d_reader.SetCurrentPos(index_info.offset);
    src.indices.reserve(index_info.num_elements);
    if(index_info.size==2) {
        for(int i=0; i<index_info.num_elements; ++i) {
            src.indices.push_back(e3d_reader.GetU2());
        }
    } else {
        for(int i=0; i<index_info.num_elements; ++i) {
            src.indices.push_back(e3d_reader.GetU4());
        }
    }
}
void collectVertices(StreamReaderLE &strm, E3DObject &src, E3D_StreamInfo &info,VertexConfig &hdr)
{
    strm.SetCurrentPos(info.offset);
    src.positions.reserve(info.num_elements);
    src.uv.reserve(info.num_elements);
    if(hdr.has_normal())
        src.normals.reserve(info.num_elements);
    aiMatrix4x4 xRot;
    aiMatrix4x4::RotationX(AI_DEG_TO_RAD(-90),xRot);
    if(hdr.has_secondary_texture_coordinate()) {
        DefaultLogger::get()->warn("Object has secondary texture coordinates... ignoring");
    }
    for(int i=0; i<info.num_elements; ++i) {
        aiVector2D uv;
        if (hdr.half_uv())
        {
            uv.x = half_to_float(strm.GetU2());
            uv.y = half_to_float(strm.GetU2());
        }
        else
        {
            uv.x = strm.GetF4();
            uv.y = strm.GetF4();
        }
        src.uv.push_back(uv);
        if (hdr.has_secondary_texture_coordinate())
        {
            if (hdr.half_extra_uv())
                strm.IncPtr(2*sizeof(uint16_t)); // unused ??
            else
                strm.IncPtr(2*sizeof(float)); // unused ??
        }
        if (hdr.has_normal())
        {
            aiVector3D norm;
            if (hdr.compressed_normal())
            {
                float temp[3];
                uncompress_normal(strm.GetU2(), temp);
                norm = aiVector3D(temp[0],temp[1],temp[2]);
            }
            else
            {
                norm.x = strm.GetF4();
                norm.y = strm.GetF4();
                norm.z = strm.GetF4();
            }
            src.normals.push_back((xRot*norm).Normalize());
        }
        if (hdr.has_tangent())
        {
            if (hdr.compressed_normal())
                strm.IncPtr(sizeof(uint16_t)); // unused ??
            else
                strm.IncPtr(3*sizeof(float)); // unused ??
        }
        aiVector3D pos;
        if (hdr.half_position())
        {
            pos.x = half_to_float(strm.GetU2());
            pos.y = half_to_float(strm.GetU2());
            pos.z = half_to_float(strm.GetU2());
        }
        else
        {
            pos.x = strm.GetF4();
            pos.y = strm.GetF4();
            pos.z = strm.GetF4();
        }
        src.positions.push_back(xRot*pos);
        if(hdr.has_color()) {
            aiColor4D c;
            c.r = float(strm.GetU1())/255.0;
            c.g = float(strm.GetU1())/255.0;
            c.b = float(strm.GetU1())/255.0;
            c.a = float(strm.GetU1())/255.0;
            src.colors.push_back(c);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Imports the given file into the given scene structure.
void SkeletonImporter::InternReadFile( const std::string& pFile, aiScene* pScene,
                                       IOSystem* pIOHandler)
{
    std::shared_ptr<IOStream> file( pIOHandler->Open( pFile, "rb"));

    // Check whether we can read from the file
    if( file.get() == NULL) {
        throw DeadlyImportError( "Failed to open DUMMY file " + pFile + ".");
    }
    StreamReaderLE e3d_reader(file);
    uint8_t version_number[4];
    e3d_reader.SetCurrentPos(4); // skip magic token
    e3d_reader.CopyAndAdvance(version_number,4); // read in the version number
    e3d_reader.IncPtr(16); // skip md5
    int header_offset = e3d_reader.GetI4();
    e3d_reader.SetCurrentPos(header_offset);
    E3D_StreamInfo vertex_info;
    E3D_StreamInfo index_info;
    E3D_StreamInfo material_info;
    readStreamInfo(e3d_reader,vertex_info);
    readStreamInfo(e3d_reader,index_info);
    readStreamInfo(e3d_reader,material_info);
    VertexConfig v_config;
    v_config.vertex_options = e3d_reader.GetU1();
    v_config.vertex_format = e3d_reader.GetU1();
    e3d_reader.IncPtr(2); // skip 2 reserved bytes
    std::ostringstream vers_str;
    dumpStreamInfo("vertex",vertex_info);
    dumpStreamInfo("index",index_info);
    dumpStreamInfo("material",material_info);
    vers_str << "E3d file version " << version_number[0] << '.' << version_number[1] << '.'
             <<version_number[2] <<'.' << version_number[3];
    DefaultLogger::get()->warn(vers_str.str());
    if ((version_number[0] == 1) && (version_number[1] == 1))
    {
        if ((v_config.vertex_options & 0xF0) != 0)
        {
            throw DeadlyImportError( "Unkown vertex options in file " + pFile + ".");
        }

        if ((v_config.vertex_format & 0xE0) != 0)
        {
            throw DeadlyImportError( "Unkown vertex format in file " + pFile + ".");
        }
    }
    else
    {
        if ((version_number[0] == 1) && (version_number[1] == 0))
        {
            v_config.vertex_format = 0;
            v_config.vertex_options ^= 0x01;
            v_config.vertex_options &= 0x07;
        }
        else
        {
            throw DeadlyImportError( "Unkown version number in file " + pFile + ".");
        }
    }

    if (v_config.check_vertex_size() != vertex_info.size)
    {
        throw DeadlyImportError( "Wrong vertex size in file " + pFile + ".");
    }

    if (v_config.get_material_size() != material_info.size)
    {
        throw DeadlyImportError( "Wrong material size in file " + pFile + ".");
    }
    if (v_config.short_index())
    {
        if (index_info.size != sizeof(uint16_t))
            throw DeadlyImportError( "Wrong index size in file " + pFile + ".");
    }
    else
    {
        if (index_info.size != sizeof(uint32_t))
            throw DeadlyImportError( "Wrong index size in file " + pFile + ".");
    }

    // allocate storage and copy the contents of the file to a memory buffer
    // First find out how many vertices we'll need
    // allocate storage for the output vertices
    // second: now parse all face indices
    // generate the output node graph
    pScene->mRootNode = new aiNode();
    pScene->mRootNode->mName.Set(DefaultIOSystem::completeBaseName(pFile));

    E3DObject src;
    if(index_info.size!=2 && index_info.size!=4) {
        throw DeadlyImportError( "Unsupported index element size in file " + pFile + ".");
    }
    collectIndices(e3d_reader, src, index_info);
    collectVertices(e3d_reader, src, vertex_info,v_config);
    // generate a default material
    generateMaterialsSplitMeshes(pScene,e3d_reader,material_info,src);
//    aiMatrix4x4::RotationX(AI_DEG_TO_RAD(90),pScene->mRootNode->mTransformation);
//    pScene->mRootNode->mTransformation = aiMatrix4x4(1.f, 0.f,0.f,0.f,
//                                                     0.f, 0.f,1.f,0.f,
//                                                     0.f,-1.f,0.f,0.f,
//                                                     0.f, 0.f,0.f,1.f);
}
