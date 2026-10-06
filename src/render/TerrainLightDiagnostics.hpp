#pragma once
#include <cmath>
namespace wxl::features::heightblend {
// Native terrain VS c28..c36: three camera-relative position, colour,
// attenuation triplets. Inspect only; never changes selection or constants.
inline unsigned NearbyTerrainLights(const float* slots) {
    unsigned count=0;
    for(unsigned i=0;i<3;++i) {
        const float* s=slots+i*12;
        bool finite=true; for(unsigned j=0;j<12;++j) finite=finite && std::isfinite(s[j]);
        const double d=double(s[0])*s[0]+double(s[1])*s[1]+double(s[2])*s[2];
        if(finite && d<=900 && s[4]>=0 && s[5]>=0 && s[6]>=0 &&
           s[4]+s[5]+s[6]>0 && s[8]>=0 && s[9]>=0 && s[10]>=0) ++count;
    }
    return count;
}
}
