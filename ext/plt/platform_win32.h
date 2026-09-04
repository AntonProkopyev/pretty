#pragma once

#include "platform.h"

namespace plt {
    Platform* createWin32Platform(stl::ObjPool& owner);
}
