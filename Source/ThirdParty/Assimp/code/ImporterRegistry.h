/*
---------------------------------------------------------------------------
Open Asset Import Library (assimp)
---------------------------------------------------------------------------

Copyright (c) 2006-2015, assimp team

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

/** @file ImporterRegistry.h

Central registry for all available importers.
*/

#pragma once
#include <vector>
#include <functional>
namespace Assimp {
#define DEFINE_FORMAT(klass,initname) \
    class klass##Registrar { \
    public: \
        klass##Registrar(int x) \
        { \
            ImporterRegistry::GetInstance().Register( klass##Registrar::create); \
        } \
        static BaseImporter *create() { \
            return new klass(); \
        } \
        static klass##Registrar s_##klass##Registrar;\
    }; \
    namespace Assimp {\
    void init_##initname() {\
        klass##Registrar klass##Registrar(0);\
    }\
}
// for formats without loader
#define DEFINE_NULL_FORMAT(initname) \
    namespace Assimp {\
    void init_##initname() {\
    }\
}
#define REGISTER_FORMAT(name)\
    extern void init_##name();\
    init_##name();


class BaseImporter;
struct ImporterRegistry {
public:

    static ImporterRegistry& GetInstance() {
        static ImporterRegistry* Instance = nullptr;
        if(!Instance)
            Instance = new ImporterRegistry();
        return *Instance;
    }

    void Register( BaseImporter * (*construct)() ) {
        m_constructors.push_back(construct);
    }
    void GetImporterInstanceList(std::vector< BaseImporter* >& out) {
        out.reserve(m_constructors.size());
        for(auto &func : m_constructors) {
            out.push_back(func());
        }
    }

private:
    std::vector< BaseImporter *(*)() > m_constructors;
    ImporterRegistry()  { ; }
   ~ImporterRegistry()  { ; }
};

}
