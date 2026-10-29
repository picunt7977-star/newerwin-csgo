#pragma once

#include <Utils/Pad.h>

#include "Vector.h"

namespace csgo
{

struct ViewSetup {
    PAD(172);
    void* csm;
    float fov;
    float fovViewmodel;
    Vector origin;
    Vector angles;
    float nearZ;
    float farZ;
};

}
