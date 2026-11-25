// Exactly one 256-lane group owns this entire bounded cloth. Every color covers
// disjoint read/write vertices. No cross-group synchronization is required.
void solveClothGroupedTick(uint lane){
    if(vertexCount>512||lambdaCount>4608)return; // uniform capacity guard
    for(uint id=lane;id<vertexCount;id+=256){Vertex v=vertices[id];clothStorePosition(id,v.position.xyz);groupStoreVelocity(id,v.velocity.xyz);}
    GroupMemoryBarrierWithGroupSync();
    precise float3 acceleration[2],previous[2];
    for(uint substep=0;substep<solverSubsteps;++substep){
        for(uint id=lane;id<vertexCount;id+=256){
            precise float3 a=clothAcceleration(id);
            acceleration[id/256]=a;aerodynamics[id]=float4(a,0);
        }
        // All neighbor position/velocity reads finish before any integration.
        GroupMemoryBarrierWithGroupSync();
        for(uint id=lane;id<vertexCount;id+=256){
            precise float3 p=clothPosition(id),v=groupVelocity(id);previous[id/256]=p;
            v=clothIntegratedVelocity(id,v,acceleration[id/256]);p=clothIntegratedPosition(id,p,v);
            clothStorePosition(id,p);groupStoreVelocity(id,v);
        }
        for(uint key=lane;key<lambdaCount;key+=256)groupLambdas[key]=0;
        GroupMemoryBarrierWithGroupSync();
        for(uint iteration=0;iteration<solverIterations;++iteration){
            for(uint color=0;color<membraneColorCount;++color){
                uint first=schedule[colorScheduleBase+color*2],count=schedule[colorScheduleBase+color*2+1];
                for(uint item=lane;item<count;item+=256)solveClothMembrane(schedule[first+item]);
                GroupMemoryBarrierWithGroupSync();
            }
            for(uint color=0;color<hingeColorCount;++color){
                uint offset=colorScheduleBase+(membraneColorCount+color)*2,first=schedule[offset],count=schedule[offset+1];
                for(uint item=lane;item<count;item+=256)solveClothHinge(schedule[first+item]);
                GroupMemoryBarrierWithGroupSync();
            }
            for(uint id=lane;id<vertexCount;id+=256)clothStorePosition(id,projectClothContacts(id,clothPosition(id)));
            GroupMemoryBarrierWithGroupSync();
        }
        // Preserve the general backend's final contact and last-substep velocity.
        for(uint id=lane;id<vertexCount;id+=256){
            precise float3 p=projectClothContacts(id,clothPosition(id));clothStorePosition(id,p);
            precise float3 v=clothProjectedVelocity(id,p,previous[id/256]);
            if(any(!isfinite(p))||any(!isfinite(v))){countStat(0,1);p=rest[id].positionWeight.xyz+origin;previous[id/256]=p;v=0;clothStorePosition(id,p);}
            groupStoreVelocity(id,v);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    for(uint id=lane;id<vertexCount;id+=256){Vertex v=vertices[id];v.position=float4(clothPosition(id),1);v.previous=float4(previous[id/256],1);v.velocity=float4(groupVelocity(id),0);vertices[id]=v;}
    for(uint key=lane;key<lambdaCount;key+=256)lambdas[key]=groupLambdas[key];
}

#if defined(CLOTH_BATCH)
[numthreads(256,1,1)]
void CSBatchTick(uint lane:SV_GroupIndex,uint3 group:SV_GroupID){
    if(group.x>=batchRecordCount)return;
    loadClothBatch(group.x);
    if(vertexCount>512||lambdaCount>4608)return;
    if(lane==0&&batchPreparedWind==0){diagnostics.Store(32,0);if(flags&1)diagnostics.Store(36,0);}
    DeviceMemoryBarrierWithGroupSync();
    solveClothGroupedTick(lane);
    DeviceMemoryBarrierWithGroupSync();
    for(uint id=lane;id<vertexCount;id+=256)updateClothNormal(id);
}
#else
[numthreads(256,1,1)]
void CSGroupedTick(uint lane:SV_GroupIndex){solveClothGroupedTick(lane);}
#endif
