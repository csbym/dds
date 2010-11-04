/** @file mongommf.h
*
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "../util/mmap.h"
#include "../util/moveablebuffer.h"

namespace mongo {

    /** MongoMMF adds some layers atop memory mapped files - specifically our handling of private views & such.
        if you don't care about journaling/durability (temp sort files & such) use MemoryMappedFile class, 
        not this.
    */
    class MongoMMF : private MemoryMappedFile { 
    public:
        MongoMMF();
        virtual ~MongoMMF();
        virtual void close();
        unsigned long long length() const { return MemoryMappedFile::length(); }
        bool open(string fname, bool sequentialHint);
        bool create(string fname, unsigned long long& len, bool sequentialHint);

        /* Get the "standard" view (which is the private one).
           We re-map the private view frequently, thus the use of MoveableBuffer 
           use.
           @return the private view
        */
        MoveableBuffer getView();

        static void* _switchToWritableView(void *private_ptr);

        /** for _DEBUG build.
            translates the read view pointer into a pointer to the corresponding 
            place in the private view.
        */
        static void* switchToPrivateView(void *debug_readonly_ptr);

    private:
        void *_view_write;
        void *_view_private;
        void *_view_readonly; // for _DEBUG build
    };

}
