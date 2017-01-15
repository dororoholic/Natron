/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2013-2017 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "PluginMemory.h"

#include <vector>
#include <cassert>
#include <stdexcept>

CLANG_DIAG_OFF(deprecated)
#include <QtCore/QMutex>
CLANG_DIAG_ON(deprecated)
#include "Engine/AppManager.h"
#include "Engine/Cache.h"
#include "Engine/RamBuffer.h"
NATRON_NAMESPACE_ENTER;

struct PluginMemory::Implementation
{
    Implementation(const EffectInstancePtr& effect_)
    : data()
    , mutex()
    , effect(effect_)
    {
    }

    RamBuffer<char> data;
    QMutex mutex;
    EffectInstanceWPtr effect;
};

PluginMemory::PluginMemory(const EffectInstancePtr& effect)
: ImageStorageBase(appPTR->getCache())
, _imp( new Implementation(effect) )
{
}

PluginMemory::~PluginMemory()
{

}

std::size_t
PluginMemory::getSize() const
{
    return _imp->data.size();
}

void
PluginMemory::allocateMemoryImpl(const AllocateMemoryArgs& args)
{
    const PluginMemAllocateMemoryArgs* thisArgs = dynamic_cast<const PluginMemAllocateMemoryArgs*>(&args);
    assert(thisArgs);

    QMutexLocker l(&_imp->mutex);
    _imp->data.resize(thisArgs->_nBytes);
}

void
PluginMemory::deallocateMemoryImpl()
{
    QMutexLocker l(&_imp->mutex);
    _imp->data.clear();

}


void*
PluginMemory::getPtr()
{
    QMutexLocker l(&_imp->mutex);

    assert( _imp->data.size() == 0 || ( _imp->data.size() > 0 && _imp->data.getData() ) );

    return (void*)( _imp->data.getData() );
}

NATRON_NAMESPACE_EXIT;
