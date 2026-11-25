// Shared arithmetic for UAV dispatches and group-shared colored XPBD. Callers
// provide clothPosition/clothStorePosition and clothLambda/clothStoreLambda.
void solveClothMembrane(uint id){
    Membrane m=membranes[id];uint3 indices=m.ids.xyz;
    precise float3 p[3]={clothPosition(indices.x),clothPosition(indices.y),clothPosition(indices.z)};
    precise float w[3]={rest[indices.x].positionWeight.w,rest[indices.y].positionWeight.w,rest[indices.z].positionWeight.w};
    precise float cu[3]={-m.inverse.x-m.inverse.y,m.inverse.x,m.inverse.y},cv[3]={-m.inverse.z-m.inverse.w,m.inverse.z,m.inverse.w};
    for(uint kind=0;kind<3;++kind){
        precise float3 u=(p[1]-p[0])*m.inverse.x+(p[2]-p[0])*m.inverse.y,v=(p[1]-p[0])*m.inverse.z+(p[2]-p[0])*m.inverse.w;precise float value;precise float3 g[3];
        if(kind<2){precise float3 f=kind==0?u:v;precise float size=length(f);if(size<1e-9){countStat(1,1);continue;}value=size-1;for(uint i=0;i<3;++i)g[i]=f*((kind==0?cu[i]:cv[i])/size);}
        else{value=dot(u,v);for(uint i=0;i<3;++i)g[i]=v*cu[i]+u*cv[i];}
        precise float denominator=0;for(uint i=0;i<3;++i)denominator+=w[i]*dot(g[i],g[i]);if(denominator<1e-12)continue;
        precise float alpha=(kind<2?stretchCompliance:shearCompliance)/(m.data.x*h*h);uint key=id*3+kind;
        precise float lambda=clothLambda(key),delta=(-value-alpha*lambda)/(denominator+alpha);clothStoreLambda(key,lambda+delta);
        for(uint i=0;i<3;++i)p[i]+=g[i]*(w[i]*delta);
    }
    clothStorePosition(indices.x,p[0]);clothStorePosition(indices.y,p[1]);clothStorePosition(indices.z,p[2]);
}
void solveClothHinge(uint id){
    Hinge hinge=hinges[id];precise float3 p[4];precise float w[4];for(uint i=0;i<4;++i){p[i]=clothPosition(hinge.ids[i]);w[i]=rest[hinge.ids[i]].positionWeight.w;}
    precise float3 e=p[1]-p[0],n0=cross(e,p[2]-p[0]),n1=cross(p[3]-p[0],e);precise float e2=dot(e,e),a=dot(n0,n0),b=dot(n1,n1);if(e2<1e-16||a<1e-20||b<1e-20){countStat(1,1);return;}precise float size=sqrt(e2),norm=sqrt(a*b);
    precise float angle=atan2(dot(cross(n0,n1),e)/(norm*size),dot(n0,n1)/norm);precise float3 g[4];g[2]=n0*(-size/a);g[3]=n1*(-size/b);g[0]=g[2]*(dot(p[2]-p[1],e)/e2)+g[3]*(dot(p[3]-p[1],e)/e2);g[1]=g[2]*(-dot(p[2]-p[0],e)/e2)+g[3]*(-dot(p[3]-p[0],e)/e2);
    precise float denominator=0;for(uint i=0;i<4;++i)denominator+=w[i]*dot(g[i],g[i]);if(denominator<1e-12)return;precise float difference=atan2(sin(angle-hinge.data.x),cos(angle-hinge.data.x));precise float alpha=bendAngleCompliance/(h*h);uint key=triangleCount*3+id;
    precise float lambda=clothLambda(key),delta=(-difference-alpha*lambda)/(denominator+alpha);clothStoreLambda(key,lambda+delta);
    for(uint i=0;i<4;++i)clothStorePosition(hinge.ids[i],p[i]+g[i]*(w[i]*delta));
}
precise float3 projectClothContacts(uint id,precise float3 p){
    if(rest[id].positionWeight.w==0)return rest[id].anchorMass.xyz+origin;
    for(uint index=0;index<colliderCount;++index){Collider c=colliders[index];uint type=(uint)c.b.w;precise float3 before=p;if(type==0){precise float distance=dot(p,c.a.xyz)-c.a.w;if(distance<thickness)p+=c.a.xyz*(thickness-distance);}
        else{precise float3 centre=c.a.xyz;if(type==2){precise float3 axis=c.b.xyz-c.a.xyz;precise float denominator=dot(axis,axis);if(denominator<1e-12)continue;centre+=axis*clamp(dot(p-c.a.xyz,axis)/denominator,0,1);}precise float3 delta=p-centre;precise float distance=length(delta),radius=c.a.w+thickness;if(distance<radius)p=centre+(distance>1e-6?delta/distance:float3(0,1,0))*radius;}
        if(any(before!=p)){countStat(11,1);countStat(12+type,1);}
    }
    return p;
}
