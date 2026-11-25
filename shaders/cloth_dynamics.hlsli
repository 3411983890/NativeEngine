precise float3 clothCurrentVelocity(uint id){
#if defined(CLOTH_GROUPED)
    return groupVelocity(id);
#else
    return vertices[id].velocity.xyz;
#endif
}
precise float3 clothAcceleration(uint id){
    precise float3 force=0;
    if(rest[id].positionWeight.w!=0){
        for(uint index=adjacency[id];index<adjacency[id+1];++index){
            uint3 t=membranes[adjacency[adjacencyBase+index]].ids.xyz;
            precise float3 normal=cross(clothPosition(t.y)-clothPosition(t.x),clothPosition(t.z)-clothPosition(t.x));precise float twiceArea=length(normal);if(twiceArea<1e-12)continue;normal/=twiceArea;
            precise float3 relative=((windAt(t.x)-clothCurrentVelocity(t.x))+(windAt(t.y)-clothCurrentVelocity(t.y))+(windAt(t.z)-clothCurrentVelocity(t.z)))/3;
            precise float speed=dot(relative,normal);force+=normal*(.5*1.225*dragCoefficient*(twiceArea*.5)*speed*abs(speed)/3);
        }
    }
    precise float3 result=force/rest[id].anchorMass.w;precise float magnitude=length(result);if(magnitude>4000)result*=4000/magnitude;return result;
}
precise float3 clothIntegratedVelocity(uint id,precise float3 velocity,precise float3 acceleration){
    if(rest[id].positionWeight.w!=0){velocity+=(float3(0,-9.81,0)+acceleration)*h;velocity/=1+damping*h;}return velocity;
}
precise float3 clothIntegratedPosition(uint id,precise float3 position,precise float3 velocity){
    if(rest[id].positionWeight.w!=0)position+=velocity*h;return position;
}
precise float3 clothProjectedVelocity(uint id,precise float3 position,precise float3 previous){return rest[id].positionWeight.w==0?0:(position-previous)/h;}
