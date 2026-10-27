#undef NDEBUG
#include "chase_camera_collision.h"
#include <cassert>
#include <limits>
#include <cstring>
#include <iostream>
using namespace grid2vr;
using namespace grid2vr::chasecamera;
int main() {
    for(float yaw:{0.f,.3f,-1.2f,3.14159265f})for(float pitch:{0.f,.4f,-.6f})for(float scale:{.6f,1.f,1.8f,10.f}) {
        const ChaseVector pivot{10,20,30};
        std::array<float,8> pose{0,0,0,1,10,22,25,1.0001f};
        const auto native=pose;
        CollisionTargetTransform transform;transform.pivot=pivot;transform.scale=scale;
        assert(OrbitChasePose(pose,pivot,{0,1,0},{1,0,0},yaw,pitch,scale,&transform.rotation));
        std::array<float,4> target{10,21,31.8f,1.0001f};const auto original=target;
        assert(transform.Apply(target) && target[3]==original[3]);
        const auto ray=ChaseRotate(transform.rotation,{original[0]-native[4],original[1]-native[5],original[2]-native[6]});
        // Hit/no-hit share one transformed aim ray. Reverting to the old target
        // or substituting raw vehicle position violates this invariant.
        assert(std::abs((target[0]-pose[4])-scale*ray.x)<.0001f);
        assert(std::abs((target[1]-pose[5])-scale*ray.y)<.0001f);
        assert(std::abs((target[2]-pose[6])-scale*ray.z)<.0001f);
        if(yaw==0 && pitch==0 && scale==1)assert(target==original && pose==native);
        std::array<float,4> centre{10,20,30,1};assert(transform.Apply(centre) && centre[0]==10 && centre[1]==20 && centre[2]==30);
    }
    for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        std::array<float,4> target{0,1,2,1};auto before=target;
        CollisionTargetTransform invalid;invalid.scale=bad;assert(!invalid.Apply(target) && target==before);
        invalid={};invalid.pivot.x=bad;assert(!invalid.Apply(target) && target==before);
        invalid={};invalid.rotation.w=bad;assert(!invalid.Apply(target) && target==before);
        invalid={};target[1]=bad;before=target;assert(!invalid.Apply(target) && !std::memcmp(target.data(),before.data(),sizeof(target)));
    }
    std::cout<<"Collision aim follows vehicle-centred orbit/zoom; invalid transforms preserve output.\n";
}
