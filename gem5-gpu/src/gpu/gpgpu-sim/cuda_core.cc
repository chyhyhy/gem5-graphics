/*
 * Copyright (c) 2011 Mark D. Hill and David A. Wood
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <cmath>
#include <iostream>
#include <map>

#include "cpu/translation.hh"
#include "debug/CudaCore.hh"
#include "debug/CudaCoreAccess.hh"
#include "debug/CudaCoreFetch.hh"
#include "gpu/gpgpu-sim/cuda_core.hh"
#include "gpu/gpgpu-sim/cuda_gpu.hh"
#include "mem/page_table.hh"
#include "params/CudaCore.hh"
#include "sim/system.hh"

using namespace std;

CudaCore::CudaCore(const Params *p) :
    MemObject(p), instPort(name() + ".inst_port", this, CorePortType::Inst),
    texPort(name()+ ".tex_port", this, CorePortType::Tex),
    lsqControlPort(name() + ".lsq_ctrl_port", this),
    texControlPort(name() + ".tex_ctrl_port", this), 
    constControlPort(name() + ".const_ctrl_port", this),
    zControlPort(name() + ".z_ctrl_port", this), 
    vpoWritePort(name() + ".vpo_write_port", this),
    _params(p),
    dataMasterId(p->sys->getMasterId(name() + ".data")),
    instMasterId(p->sys->getMasterId(name() + ".inst")),
    texMasterId(p->sys->getMasterId(name() +  ".tex")), 
    constMasterId(p->sys->getMasterId(name() +  ".const")), 
    zMasterId(p->sys->getMasterId(name() +  ".z")), 
    vpoVertWriteMasterId(p->sys->getMasterId(name() +  ".vpo_vert_write")), 
    id(p->id),
    itb(p->itb), ttb(p->ttb),
    cudaGPU(p->gpu), 
    maxNumWarpsPerCore(p->warp_contexts)
{
    writebackBlocked[LSQCntrlPortType::LSQ] = -1; // Writeback is not blocked
    writebackBlocked[LSQCntrlPortType::TEX] = -1;
    writebackBlocked[LSQCntrlPortType::CONST] = -1;
    writebackBlocked[LSQCntrlPortType::Z] = -1;

    stallOnICacheRetry = false;
    stallOnTexCacheRetry  = false;

    cudaGPU->registerCudaCore(this);

    warpSize = cudaGPU->getWarpSize();

    signalKernelFinish = false;

    if (p->port_lsq_port_connection_count != warpSize) {
        panic("Shader core lsq_port size != to warp size\n");
    }

    if (p->port_tex_lq_port_connection_count != warpSize) {
        panic("Shader core tex_lq_port size != to warp size\n");
    }

    if (p->port_const_lsq_port_connection_count != warpSize) {
        panic("Shader core const_lsq_port size != to warp size\n");
    }

    if (p->port_z_lsq_port_connection_count != warpSize) {
        panic("Shader core z_lsq_port size != to warp size\n");
    }

    // create the ports
    for (int i = 0; i < warpSize; ++i) {
        lsqPorts.push_back(new LSQPort(csprintf("%s-lsqPort%d", name(), i), this, i));
        texPorts.push_back(new LSQPort(csprintf("%s-texPort%d", name(), i), this, i));
        constPorts.push_back(new LSQPort(csprintf("%s-constPort%d", name(), i), this, i));
        zPorts.push_back(new LSQPort(csprintf("%s-zPort%d", name(), i), this, i));
    }

    activeCTAs = 0;
    sentFlushReqs = 0;
    receivedFlushResp = 0;

    needsFenceUnblock.resize(maxNumWarpsPerCore);
    for (int i = 0; i < maxNumWarpsPerCore; i++) {
        needsFenceUnblock[i] = false;
    }
}

CudaCore::~CudaCore()
{
    for (int i = 0; i < warpSize; ++i) {
        delete lsqPorts[i];
        delete texPorts[i];
        delete constPorts[i];
        delete zPorts[i];
    }
    lsqPorts.clear();
    texPorts.clear();
    constPorts.clear();
    zPorts.clear();
}

BaseMasterPort&
CudaCore::getMasterPort(const std::string &if_name, PortID idx)
{
    if (if_name == "inst_port") {
        return instPort;
    } else if (if_name == "tex_port"){
        return texPort;
    } else if (if_name == "lsq_port") {
        if (idx >= static_cast<PortID>(lsqPorts.size())) {
            panic("CudaCore::getMasterPort: unknown index %d\n", idx);
        }
        return *lsqPorts[idx];
    } else if (if_name == "tex_lq_port") {
        if (idx >= static_cast<PortID>(texPorts.size())) {
            panic("CudaCore::getMasterPort: unknown index %d\n", idx);
        }
        return * texPorts[idx];

    } else if (if_name == "const_lsq_port") {
        if (idx >= static_cast<PortID>(constPorts.size())) {
            panic("CudaCore::getMasterPort: unknown index %d\n", idx);
        }
        return * constPorts[idx];
    } else if (if_name == "z_lsq_port") {
        if (idx >= static_cast<PortID>(zPorts.size())) {
            panic("CudaCore::getMasterPort: unknown index %d\n", idx);
        }
        return * zPorts[idx];
    } else if (if_name == "lsq_ctrl_port") {
        return lsqControlPort;
    } else if (if_name == "tex_ctrl_port") {
        return texControlPort;
    } else if (if_name == "const_ctrl_port") {
        return constControlPort;
    } else if (if_name == "z_ctrl_port") {
        return zControlPort;
    } else if (if_name == "vpo_write_port") {
        return vpoWritePort;
    } else {
        return MemObject::getMasterPort(if_name, idx);
    }
}

BaseSlavePort&
CudaCore::getSlavePort(const std::string &if_name, PortID idx){
   return MemObject::getSlavePort(if_name, idx);
}

void
CudaCore::unserialize(CheckpointIn &cp)
{
    // Intentionally left blank to keep from trying to read shader header from
    // checkpoint files. Allows for restore into any number of shader cores.
    // NOTE: Cannot checkpoint during kernels
}

void CudaCore::initialize()
{
    shaderImpl = cudaGPU->getTheGPU()->get_shader(id);
}

int CudaCore::isCacheResourceAvailable(Addr addr, std::map<Addr,mem_fetch *>* busyCacheLinesMap)
{
    map<Addr,mem_fetch *>::iterator iter =
            (*busyCacheLinesMap).find(cudaGPU->addrToLine(addr));
    return iter == (*busyCacheLinesMap).end();
}

void
CudaCore::icacheFetch(Addr addr, mem_fetch *mf)
{
    xCacheFetch(addr, mf, "instruction", instMasterId, &instBusyCacheLineAddrs,
            itb, Request::INST_FETCH, 0);
}

void
CudaCore::sendInstAccess(PacketPtr pkt)
{
    assert(!stallOnICacheRetry);

    DPRINTF(CudaCoreFetch,
            "Sending inst read of %d bytes to vaddr: 0x%x\n",
            pkt->getSize(), pkt->req->getVaddr());

    if (!instPort.sendTimingReq(pkt)) {
        stallOnICacheRetry = true;
        if (pkt != retryInstPkts.front()) {
            retryInstPkts.push_back(pkt);
        }
        DPRINTF(CudaCoreFetch, "Send failed vaddr: 0x%x. Waiting: %d\n",
                pkt->req->getVaddr(), retryInstPkts.size());
    }
    numInstCacheRequests++;
}

void
CudaCore::handleInstRetry()
{
    assert(stallOnICacheRetry);
    assert(retryInstPkts.size());

    numInstCacheRetry++;

    PacketPtr retry_pkt = retryInstPkts.front();
    DPRINTF(CudaCoreFetch, "Received Inst retry, vaddr: 0x%x\n",
            retry_pkt->req->getVaddr());

    if (instPort.sendTimingReq(retry_pkt)) {
        retryInstPkts.remove(retry_pkt);

        stallOnICacheRetry = (retryInstPkts.size() > 0);
        if (stallOnICacheRetry) {
            //schedule the rest of waiting requests in the following cycle
            CachePortRetryEvent * e = new CachePortRetryEvent(this, CorePortType::Inst);
            schedule(e, clockEdge(Cycles(1)));
        }
    } else {
        //if the request fails then a retry is expected
        DPRINTF(CudaCoreFetch, "Inst retry, vaddr: 0x%x failed, waiting for another retry\n",
            retry_pkt->req->getVaddr());
    }
}

void
CudaCore::recvInstResp(PacketPtr pkt)
{
    assert(pkt->req->isInstFetch());
    map<Addr,mem_fetch *>::iterator iter =
            instBusyCacheLineAddrs.find(cudaGPU->addrToLine(pkt->req->getVaddr()));
    assert(iter != instBusyCacheLineAddrs.end());

    DPRINTF(CudaCoreFetch, "Finished fetch on vaddr 0x%x\n",
            pkt->req->getVaddr());

    shaderImpl->accept_fetch_response(iter->second);

    instBusyCacheLineAddrs.erase(iter);

    if (pkt->req) delete pkt->req;
    delete pkt;
}

bool
CudaCore::executeMemOp(const warp_inst_t &inst)
{
    assert(inst.space.get_type() == global_space ||
           inst.space.get_type() == const_space ||
           inst.space.get_type() == local_space ||
           inst.space.get_type() == tex_space ||
           inst.op == BARRIER_OP ||
           inst.op == MEMORY_BARRIER_OP);
    assert(inst.valid());

    // for debugging
    bool completed = false;

    int size = inst.data_size;
    if (inst.is_load() || inst.is_store()) {
        assert(size >= 1 && size <= 8);
    }
    size *= inst.vectorLength;
    assert(size <= 16);
    if (inst.op == BARRIER_OP || inst.op == MEMORY_BARRIER_OP) {
        string type = inst.op==BARRIER_OP? "BARRIER_OP" : "MEMORY_BARRIER_OP";
        DPRINTF(CudaCoreAccess, "executing %s\n", type);
        if (inst.active_count() != inst.warp_size()) {
            warn_once("ShaderLSQ received partial-warp fence: Assuming you know what you're doing");
        }
    }
    const int asid = 0;
    Request::Flags flags;
    Request::Flags gpuFlags;
    if (inst.isatomic()) {
        assert(inst.memory_op == memory_store);
        // Assert that gem5-gpu knows how to handle the requested atomic type.
        // TODO: When all atomic types and data sizes are implemented, remove
        assert(inst.get_atomic() == ATOMIC_INC ||
               inst.get_atomic() == ATOMIC_MAX ||
               inst.get_atomic() == ATOMIC_MIN ||
               inst.get_atomic() == ATOMIC_ADD ||
               inst.get_atomic() == ATOMIC_CAS);
        assert(inst.data_type == S32_TYPE ||
               inst.data_type == U32_TYPE ||
               inst.data_type == F32_TYPE ||
               inst.data_type == B32_TYPE);
        // GPU atomics will use the MEM_SWAP flag to indicate to Ruby that the
        // request should be passed to the cache hierarchy as secondary
        // RubyRequest_Atomic.
        // NOTE: Most GPU atomics have conditional writes and most perform some
        //       operation on loaded data before writing it back into cache.
        //       This makes them read-modify-conditional-write operations, but
        //       for ease of development, use the MEM_SWAP designator for now.
        flags.set(Request::MEM_SWAP);
    }

    if(inst.space.get_type() == tex_space){
       gpuFlags.set(Request::TEX_FETCH);
    } else if(inst.space.get_type() == const_space){
       gpuFlags.set(Request::CONST_REQUEST);
    } 
    
    if(inst.space.is_z()){
       gpuFlags.set(Request::Z_REQUEST);
    }

    if (inst.space.get_type() == const_space) {
        DPRINTF(CudaCoreAccess, "Const space: %p\n", inst.pc);
    } else if (inst.space.get_type() == local_space) {
        DPRINTF(CudaCoreAccess, "Local space: %p\n", inst.pc);
    } else if (inst.space.get_type() == param_space_local) {
        DPRINTF(CudaCoreAccess, "Param local space: %p\n", inst.pc);
    } else if (inst.space.get_type() == tex_space) {
        DPRINTF(CudaCoreAccess, "Tex space: %p\n", inst.pc);
    } else if (inst.space.is_z()) {
        DPRINTF(CudaCoreAccess, "Z space: %p\n", inst.pc);
    } else if (inst.space.is_vert()) {
        DPRINTF(CudaCoreAccess, "Vertex space: %p\n", inst.pc);
    } else {
        DPRINTF(CudaCoreAccess, "Global space: %p\n", inst.pc);
    }

    if(inst.space.is_vert()){
       //if already some packets pending then stall
       unsigned kwip = (shaderImpl->get_thread(inst.warp_id()*inst.warp_size())->get_uid_in_kernel())/MAX_WARP_SIZE;
       if(vpoWritePkts.size() > 0)
          return true;
       if(!shaderImpl->can_vert_write(kwip, inst))
          return true;
       //vertex ouput uses trivial coalescing
       assert(inst.is_store());
       std::vector<Addr> addrs;
       std::vector<GLfloat> data;
       for (int lane = 0; lane < warpSize; lane++) {
          if (inst.active(lane)) {
             int reqsCount = inst.get_mem_reqs_count(lane);
             assert(reqsCount == 1); //current format
             addrs.push_back(inst.get_addr(lane, 0));
             data.push_back(*(GLfloat*)inst.get_data(lane));
          }
       }
       assert(addrs.size() > 0);
       std::vector<Addr> addrBlocks;
       std::vector<unsigned> blockSizes;
       std::vector<uint8_t*> blockData;
       Addr currBlock = addrs[0]/cudaGPU->getSystemCachelineSize();
       addrBlocks.push_back(addrs[0]);
       blockSizes.push_back(size);
       blockData.push_back((uint8_t*)&data[0]);
       for(int a=1; a<addrs.size(); a++){
          if((addrs[a] - addrs[a-1]) != sizeof(GLfloat))
             panic("Unexpected vertex attribute addressing!\n");
          if(((addrs[a]/cudaGPU->getSystemCachelineSize())!=currBlock)){
             currBlock = addrs[a]/cudaGPU->getSystemCachelineSize();
             addrBlocks.push_back(addrs[a]);
             blockSizes.push_back(size);
             blockData.push_back((uint8_t*)&data[a]);
          } else {
             blockSizes.back() += size;
          }
       }
       assert(blockSizes.size() == addrBlocks.size());
       assert(blockData.size() == addrBlocks.size());
       bool succeeded = false;
       PacketPtr lastPacket = NULL;
       for(int i=0; i<addrBlocks.size(); i++){
          RequestPtr req = new Request(asid, addrBlocks[i], blockSizes[i], flags,
                vpoVertWriteMasterId, inst.pc, inst.warp_id());
          req->setGpuFlags(gpuFlags);
          //TODO: update when adding support to vitual translation later
          req->setPaddr(addrBlocks[i]); 
          PacketPtr pkt = new Packet(req, MemCmd::WriteReq);
          lastPacket = pkt;
          pkt->allocate();
          pkt->setData(blockData[i]);
          vpoWritePkts.push(pkt);
        }

        //assuming in-order responses
        lastAttribPkts[lastPacket] = LastAttribPkt(kwip, inst.active_count());
        printf("lastAttribPkts[%p]\n", lastPacket);
        while (vpoWritePkts.size() > 0) {
            if(vpoWritePort.sendTimingReq(vpoWritePkts.front())) {
                printf("Sent write request to VPO, addr=%lx\n", vpoWritePkts.front()->getAddr());
                vpoWritePkts.pop();
            } else break;
        }
    } else {
       for (int lane = 0; lane < warpSize; lane++) {
          if (inst.active(lane)) {
             int reqsCount = inst.get_mem_reqs_count(lane);
             //BARRIER ops may report no requests but we still have to execute them
             //TODO: find a better way to do it
             if(reqsCount == 0) {
                assert(inst.op == BARRIER_OP || inst.op == MEMORY_BARRIER_OP);
                reqsCount =1; 
             }
             for(int reqNum = 0; reqNum < reqsCount; reqNum++){
                PacketPtr pkt;
                if (inst.is_load()) {
                   Addr addr = inst.get_addr(lane, reqNum);
                   // Not all cache operators are currently supported in gem5-gpu.
                   // Verify that a supported cache operator is specified for this
                   // load instruction.
                   if (!inst.isatomic() && inst.cache_op == CACHE_GLOBAL) {
                      // If this is a load instruction that must access coherent
                      // global memory, bypass the L1 cache to avoid stale hits
                      flags.set(Request::BYPASS_L1);
                   } else if (inst.cache_op != CACHE_ALL &&
                         !(inst.isatomic() && inst.cache_op == CACHE_GLOBAL)
                         && (inst.space.get_type() != tex_space)) {
                      panic("Unhandled cache operator (%d) on load\n",
                            inst.cache_op);
                   }
                   MasterID reqMasterId = dataMasterId;
                   if(inst.space.get_type() == tex_space)
                      reqMasterId = texMasterId;
                   if(inst.space.get_type() == const_space)
                      reqMasterId = constMasterId;
                   if(inst.space.is_z())
                      reqMasterId = zMasterId;
                   RequestPtr req = new Request(asid, addr, size, flags,
                         reqMasterId, inst.pc, inst.warp_id());
                   pkt = new Packet(req, MemCmd::ReadReq);
                   if (inst.isatomic()) {
                      assert(0); //should be fixed for classic memory
                      assert(flags.isSet(Request::MEM_SWAP));
                      AtomicOpRequest *pkt_data = new AtomicOpRequest();
                      pkt_data->lastAccess = true;
                      pkt_data->uniqueId = lane;
                      pkt_data->dataType = getDataType(inst.data_type);
                      pkt_data->atomicOp = getAtomOpType(inst.get_atomic());
                      pkt_data->lineOffset = 0;
                      pkt_data->setData((uint8_t*)inst.get_data(lane));

                      // TODO: If supporting atomics that require more operands,
                      // will need to copy that data here also

                      // Create packet data to include the atomic type and
                      // the register data to be used (e.g. atomicInc requires
                      // the saturating value up to which to count)
                      pkt->dataDynamic(pkt_data);
                   } else {
                      pkt->allocate();
                   }
                   // Since only loads return to the CudaCore
                   pkt->senderState = new SenderState(inst);
                } else if (inst.is_store()) {
                   assert(!inst.isatomic());
                   // Not all cache operators are currently supported in gem5-gpu.
                   // Verify that a supported cache operator is specified for this
                   // load instruction.
                   if (inst.cache_op == CACHE_GLOBAL) {
                      flags.set(Request::BYPASS_L1);
                   } else if (inst.cache_op != CACHE_ALL &&
                         inst.cache_op != CACHE_WRITE_BACK) {
                      panic("Unhandled cache operator (%d) on store\n",
                            inst.cache_op);
                   }
                   Addr addr = inst.get_addr(lane, reqNum);
                   assert(inst.space.get_type() != tex_space);
                   assert(inst.space.get_type() != const_space);
                   MasterID reqMasterId = dataMasterId;
                   if(inst.space.is_z())
                      reqMasterId = zMasterId;
                   RequestPtr req = new Request(asid, addr, size, flags,
                         reqMasterId, inst.pc, inst.warp_id());
                   pkt = new Packet(req, MemCmd::WriteReq);
                   pkt->allocate();
                   pkt->setData((uint8_t*)inst.get_data(lane));

                   DPRINTF(CudaCoreAccess, "Send store from lane %d address 0x%llx: data = %d\n",
                         lane, pkt->req->getVaddr(), *(int*)inst.get_data(lane));
                } else if (inst.op == BARRIER_OP || inst.op == MEMORY_BARRIER_OP) {
                   assert(!inst.isatomic());
                   // Setup Fence packet
                   // TODO: If adding fencing functionality, specify control data
                   // in packet or request
                   RequestPtr req = new Request(asid, 0x0, 0, flags, dataMasterId,
                         inst.pc, inst.warp_id());
                   pkt = new Packet(req, MemCmd::FenceReq);
                   pkt->senderState = new SenderState(inst);
                } else {
                   panic("Unsupported instruction type\n");
                }

                LSQPort * sendPort;
                if(inst.space.get_type() == tex_space){
                   sendPort = texPorts[lane];
                } else if(inst.space.get_type() == const_space){
                   sendPort = constPorts[lane];
                } else if(inst.space.is_z()) {
                   sendPort = zPorts[lane];
                } else {
                   sendPort = lsqPorts[lane];
                }
                pkt->req->setGpuFlags(gpuFlags);

                std::string ttype = inst.is_load()? "load" : inst.is_store()? "store": "barrier";
                DPRINTF(CudaCoreAccess, "Sent a %s request from lane %d\n", ttype.c_str(), lane);
                if (!sendPort->sendTimingReq(pkt)) {
                   // NOTE: This should fail early. If executeMemOp fails after
                   // some, but not all, of the requests have been sent the
                   // behavior is undefined.
                   if (completed) {
                      panic("Should never fail after first accepted lane");
                   }

                   if (inst.is_load() || inst.op == BARRIER_OP ||
                         inst.op == MEMORY_BARRIER_OP) {
                      delete pkt->senderState;
                   }
                   delete pkt->req;
                   delete pkt;

                   // Return that there is a pipeline stall
                   return true;
                } else {
                   completed = true;
                }
             }
          }
       }
    }

    if (inst.op == BARRIER_OP || inst.op == MEMORY_BARRIER_OP) {
        needsFenceUnblock[inst.warp_id()] = true;
    }

    // Return that there should not be a pipeline stall
    return false;
}

bool
CudaCore::recvLSQDataResp(PacketPtr pkt, int lane_id)
{
    assert(pkt->isRead() || pkt->cmd == MemCmd::FenceResp);

    DPRINTF(CudaCoreAccess, "Got a response for lane %d address 0x%llx\n",
            lane_id, pkt->req->getVaddr());

    warp_inst_t &inst = ((SenderState*)pkt->senderState)->inst;
    assert(!inst.empty() && inst.valid());

    if (pkt->isRead()) {
        if (!shaderImpl->ldst_unit_wb_inst(inst)) {
            // Writeback register is occupied, stall
            Request::Flags gpuFlags = pkt->req->getGpuFlags();
            if(gpuFlags.isSet(Request::TEX_FETCH)){
                DPRINTF(CudaCoreAccess, "Setting tex port blocked at lane %d\n", lane_id);
                assert(writebackBlocked[LSQCntrlPortType::TEX] < 0);
                writebackBlocked[LSQCntrlPortType::TEX] = lane_id;
            } else if(gpuFlags.isSet(Request::Z_REQUEST)){
                DPRINTF(CudaCoreAccess, "Setting z port blocked at lane %d\n", lane_id);
                assert(writebackBlocked[LSQCntrlPortType::Z] < 0);
                writebackBlocked[LSQCntrlPortType::Z] = lane_id;
            } else if(gpuFlags.isSet(Request::CONST_REQUEST)){
                DPRINTF(CudaCoreAccess, "Setting const port blocked at lane %d\n", 
                      lane_id);
                assert(writebackBlocked[LSQCntrlPortType::CONST] < 0);
                writebackBlocked[LSQCntrlPortType::CONST] = lane_id;
            } else {
                DPRINTF(CudaCoreAccess, "Setting lsq blocked at lane %d\n", lane_id);
                assert(writebackBlocked[LSQCntrlPortType::LSQ] < 0);
                writebackBlocked[LSQCntrlPortType::LSQ] = lane_id;
            }
            return false;
        }

        uint8_t data[16];
        assert(pkt->getSize() <= sizeof(data));

        if (inst.isatomic()) {
            assert(pkt->req->isSwap());
            AtomicOpRequest *lane_req = pkt->getPtr<AtomicOpRequest>();
            lane_req->writeData(data);
        } else {
            pkt->writeData(data);
        }
        DPRINTF(CudaCoreAccess, "Loaded data %d\n", *(int*)data);
        shaderImpl->writeRegister(inst, warpSize, lane_id, (char*)data);
    } else if (pkt->cmd == MemCmd::FenceResp) {
        if (needsFenceUnblock[inst.warp_id()]) {
            if (inst.op == BARRIER_OP) {
                // Signal that warp has reached barrier
                assert(!shaderImpl->warp_waiting_at_barrier(inst.warp_id()));
                shaderImpl->warp_reaches_barrier(inst);
                DPRINTF(CudaCoreAccess, "Warp %d reaches barrier\n",
                        pkt->req->contextId());
            }

            // Signal that fence has been cleared
            assert(shaderImpl->fence_unblock_needed(inst.warp_id()));
            shaderImpl->complete_fence(pkt->req->contextId());
            DPRINTF(CudaCoreAccess, "Cleared fence, unblocking warp %d\n",
                    pkt->req->contextId());

            needsFenceUnblock[inst.warp_id()] = false;
        }
    }

    delete pkt->senderState;
    delete pkt->req;
    delete pkt;

    return true;
}

void
CudaCore::recvLSQControlResp(PacketPtr pkt)
{
    if (pkt->isFlush()) {
        receivedFlushResp++;
        DPRINTF(CudaCoreAccess, "Got flush response %d out of %d sent\n", receivedFlushResp, sentFlushReqs);
        if (signalKernelFinish and (receivedFlushResp==sentFlushReqs)) {
            shaderImpl->finish_kernel();
            signalKernelFinish = false;
            sentFlushReqs = 0;
            receivedFlushResp = 0;
        }
    } else {
        panic("Received unhandled packet type in control port");
    }
    delete pkt->req;
    delete pkt;
}

void
CudaCore::writebackClear()
{
   if (writebackBlocked[LSQCntrlPortType::LSQ] >= 0){
       lsqPorts[writebackBlocked[LSQCntrlPortType::LSQ]]->sendRetryResp();
       writebackBlocked[LSQCntrlPortType::LSQ] = -1;
    }

    if (writebackBlocked[LSQCntrlPortType::TEX] >= 0){
       texPorts[writebackBlocked[LSQCntrlPortType::TEX]]->sendRetryResp();
       writebackBlocked[LSQCntrlPortType::TEX] = -1;
    }

    if (writebackBlocked[LSQCntrlPortType::CONST] >= 0){
       constPorts[writebackBlocked[LSQCntrlPortType::CONST]]->sendRetryResp();
       writebackBlocked[LSQCntrlPortType::CONST] = -1;
    }

    if (writebackBlocked[LSQCntrlPortType::Z] >= 0){
       zPorts[writebackBlocked[LSQCntrlPortType::Z]]->sendRetryResp();
       writebackBlocked[LSQCntrlPortType::Z] = -1;
    }
}

void
CudaCore::flush()
{
    int asid = 0;
    Addr addr(0);
    Request::Flags flags;
    RequestPtr req = new Request(asid, addr, flags, dataMasterId);
    PacketPtr pkt = new Packet(req, MemCmd::FlushReq);

    sentFlushReqs+=3;
    
    DPRINTF(CudaCoreAccess, "Sending flush requests\n");
    if (!lsqControlPort.sendTimingReq(pkt)){
        panic("Flush requests should never fail");
    }
    
    RequestPtr texReq = new Request(asid, addr, flags, texMasterId);
    PacketPtr texPkt = new Packet(texReq, MemCmd::FlushReq);
    if (!texControlPort.sendTimingReq(texPkt)){
        panic("Flush requests should never fail");
    }

    RequestPtr constReq = new Request(asid, addr, flags, constMasterId);
    PacketPtr constPkt = new Packet(constReq, MemCmd::FlushReq);
    if (!constControlPort.sendTimingReq(constPkt)){
        panic("Flush requests should never fail");
    }

    RequestPtr zReq = new Request(asid, addr, flags, zMasterId);
    PacketPtr zPkt = new Packet(zReq, MemCmd::FlushReq);
    if (!zControlPort.sendTimingReq(zPkt)){
        panic("Flush requests should never fail");
    }

}

void
CudaCore::finishKernel()
{
    numKernelsCompleted++;
    signalKernelFinish = true;
    flush();
}

bool
CudaCore::LSQPort::recvTimingResp(PacketPtr pkt)
{
    return core->recvLSQDataResp(pkt, idx);
}

void
CudaCore::LSQPort::recvReqRetry()
{
    panic("Not sure how to respond to a recvReqRetry...");
}

bool
CudaCore::LSQControlPort::recvTimingResp(PacketPtr pkt)
{
    core->recvLSQControlResp(pkt);
    return true;
}

void
CudaCore::LSQControlPort::recvReqRetry()
{
    panic("CudaCore::LSQControlPort::recvReqRetry() not implemented!");
}

bool
CudaCore::recvVpoTimingResp(PacketPtr pkt){
   if(lastAttribPkts.find(pkt)!=lastAttribPkts.end()){
      LastAttribPkt& lap = lastAttribPkts[pkt];
      shaderImpl->signal_attrib_done(lap.warpId, lap.activeCount);
      lastAttribPkts.erase(pkt);
   }

   assert(pkt->isWrite());
   delete pkt->req;
   delete pkt;
   return true;
}

bool
CudaCore::VpoMasterPort::recvTimingResp(PacketPtr pkt)
{
   return core->recvVpoTimingResp(pkt);
}

void
CudaCore::recvVpoReqRetry(){
   while(vpoWritePkts.size() > 0){
      if(vpoWritePort.sendTimingReq(vpoWritePkts.front())) {
         DPRINTF(CudaCoreAccess, "Sent a delayed write request to VPO, addr=%llx\n", vpoWritePkts.front()->getAddr());
         vpoWritePkts.pop();
      } else break;
   }
}

void
CudaCore::VpoMasterPort::recvReqRetry()
{
   core->recvVpoReqRetry();
}

Tick
CudaCore::VpoMasterPort::recvAtomic(PacketPtr pkt)
{
   panic("Not sure how to recvAtomic");
   return 0;
}

void
CudaCore::VpoMasterPort::recvFunctional(PacketPtr pkt)
{
   panic("Not sure how to recvFunctional");
}

bool
CudaCore::CoreCachePort::recvTimingResp(PacketPtr pkt)
{
    if(type == CorePortType::Inst){
       core->recvInstResp(pkt);
    } else if(type == CorePortType::Tex){
       core->recvTexResp(pkt);
    } else {
       fatal("Unexpectec port type");
    }

    return true;
}

void
CudaCore::CoreCachePort::recvReqRetry()
{
    if(type == CorePortType::Inst){
       core->handleInstRetry();
    } else if (type == CorePortType::Tex){
       core->handleTexRetry();
    } else {
       fatal("Unexpected port type");
    }
}

Tick
CudaCore::CoreCachePort::recvAtomic(PacketPtr pkt)
{
    panic("Not sure how to recvAtomic");
    return 0;
}


void
CudaCore::CoreCachePort::recvFunctional(PacketPtr pkt)
{
    panic("Not sure how to recvFunctional");
}

CudaCore *CudaCoreParams::create() {
    return new CudaCore(this);
}

void
CudaCore::regStats()
{
    MemObject::regStats();

    numLocalLoads
        .name(name() + ".local_loads")
        .desc("Number of loads from local space")
        ;
    numLocalStores
        .name(name() + ".local_stores")
        .desc("Number of stores to local space")
        ;
    numSharedLoads
        .name(name() + ".shared_loads")
        .desc("Number of loads from shared space")
        ;
    numSharedStores
        .name(name() + ".shared_stores")
        .desc("Number of stores to shared space")
        ;
    numParamKernelLoads
        .name(name() + ".param_kernel_loads")
        .desc("Number of loads from kernel parameter space")
        ;
    numParamLocalLoads
        .name(name() + ".param_local_loads")
        .desc("Number of loads from local parameter space")
        ;
    numParamLocalStores
        .name(name() + ".param_local_stores")
        .desc("Number of stores to local parameter space")
        ;
    numConstLoads
        .name(name() + ".const_loads")
        .desc("Number of loads from constant space")
        ;
    numTexLoads
        .name(name() + ".tex_loads")
        .desc("Number of loads from texture space")
        ;
    numZLoads
        .name(name() + ".z_loads")
        .desc("Number of loads from z space")
        ;
    numZStores
        .name(name() + ".z_stores")
        .desc("Number of stores to z space")
        ;
    numGlobalLoads
        .name(name() + ".global_loads")
        .desc("Number of loads from global space")
        ;
    numGlobalStores
        .name(name() + ".global_stores")
        .desc("Number of stores to global space")
        ;
    numSurfLoads
        .name(name() + ".surf_loads")
        .desc("Number of loads from surface space")
        ;
    numGenericLoads
        .name(name() + ".generic_loads")
        .desc("Number of loads from generic spaces (global, shared, local)")
        ;
    numGenericStores
        .name(name() + ".generic_stores")
        .desc("Number of stores to generic spaces (global, shared, local)")
        ;
    numInstCacheRequests
        .name(name() + ".inst_cache_requests")
        .desc("Number of instruction cache requests sent")
        ;
    numInstCacheRetry
        .name(name() + ".inst_cache_retries")
        .desc("Number of instruction cache retries")
        ;
    instCounts
        .init(8)
        .name(name() + ".inst_counts")
        .desc("Inst counts: 1: ALU, 2: MAD, 3: CTRL, 4: SFU, 5: MEM, 6: TEX, 7: NOP")
        ;
    numTexCacheRequests
        .name(name() + ".tex_cache_requests")
        .desc("Number of texture cache requests sent")
        ;
    numTexCacheRetry
        .name(name() + ".tex_cache_retries")
        .desc("Number of texture cache retries")
        ;
    activeCycles
        .name(name() + ".activeCycles")
        .desc("Number of cycles this shader was executing a CTA")
        ;
    notStalledCycles
        .name(name() + ".notStalledCycles")
        .desc("Number of cycles this shader was actually executing at least one instance")
        ;
    instInstances
        .name(name() + ".instInstances")
        .desc("Total instructions executed by all PEs in the core")
        ;
    instPerCycle
        .name(name() + ".instPerCycle")
        .desc("Instruction instances per cycle")
        ;

    instPerCycle = instInstances / activeCycles;
    numKernelsCompleted
        .name(name() + ".kernels_completed")
        .desc("Number of kernels completed")
        ;
}

void
CudaCore::record_ld(memory_space_t space)
{
    switch(space.get_type()) {
    case local_space: numLocalLoads++; break;
    case shared_space: numSharedLoads++; break;
    case param_space_kernel: numParamKernelLoads++; break;
    case param_space_local: numParamLocalLoads++; break;
    case const_space: numConstLoads++; break;
    case tex_space: numTexLoads++; break;
    case surf_space: numSurfLoads++; break;
    case global_space: numGlobalLoads++; break;
    case generic_space: numGenericLoads++; break;
    case z_space: numZLoads++; break;
    case param_space_unclassified:
    case undefined_space:
    case reg_space:
    case instruction_space:
    default:
        panic("Load from invalid space: %d!", space.get_type());
        break;
    }
}

void
CudaCore::record_st(memory_space_t space)
{
    switch(space.get_type()) {
    case local_space: numLocalStores++; break;
    case shared_space: numSharedStores++; break;
    case param_space_local: numParamLocalStores++; break;
    case global_space: numGlobalStores++; break;
    case generic_space: numGenericStores++; break;
    case z_space: numZStores++; break;

    case param_space_kernel:
    case const_space:
    case tex_space:
    case surf_space:
    case param_space_unclassified:
    case undefined_space:
    case reg_space:
    case instruction_space:
    default:
        panic("Store to invalid space: %d!", space.get_type());
        break;
    }
}

void
CudaCore::record_inst(int inst_type)
{
    instCounts[inst_type]++;

    // if not nop
    if (inst_type != 7) {
        instInstances++;
        if (curCycle() != lastActiveCycle) {
            lastActiveCycle = curCycle();
            notStalledCycles++;
        }
    }
}

void
CudaCore::record_block_issue(unsigned hw_cta_id)
{
    assert(!coreCTAActive[hw_cta_id]);
    coreCTAActive[hw_cta_id] = true;
    coreCTAActiveStats[hw_cta_id].push_back(curTick());

    if (activeCTAs == 0) {
        beginActiveCycle = curCycle();
    }
    activeCTAs++;
}

void
CudaCore::record_block_commit(unsigned hw_cta_id)
{
    assert(coreCTAActive[hw_cta_id]);
    coreCTAActive[hw_cta_id] = false;
    coreCTAActiveStats[hw_cta_id].push_back(curTick());

    activeCTAs--;
    if (activeCTAs == 0) {
        activeCycles += curCycle() - beginActiveCycle;
    }
}

void
CudaCore::printCTAStats(std::ostream& out)
{
    std::map<unsigned, std::vector<Tick> >::iterator iter =
            coreCTAActiveStats.begin();
    std::vector<Tick>::iterator times;
    for (; iter != coreCTAActiveStats.end(); iter++) {
        unsigned cta_id = iter->first;
        out << id << ", " << cta_id << ", ";
        times = coreCTAActiveStats[cta_id].begin();
        for (; times != coreCTAActiveStats[cta_id].end(); times++) {
            out << *times << ", ";
        }
        out << curTick() << "\n";
    }
}

void
CudaCore::sendTexAccess(PacketPtr pkt)
{
    assert(!stallOnTexCacheRetry);
    assert(pkt->req->isBypassL1());

    DPRINTF(CudaCoreFetch,
            "Sending tex fetch of %d bytes to vaddr: 0x%x\n",
            pkt->getSize(), pkt->req->getVaddr());

    if (!texPort.sendTimingReq(pkt)) {
        stallOnTexCacheRetry = true;
        if (pkt != retryTexPkts.front()) {
            retryTexPkts.push_back(pkt);
        }
        DPRINTF(CudaCoreFetch, "Send failed vaddr: 0x%x. Waiting: %d\n",
                pkt->req->getVaddr(), retryTexPkts.size());
    }
    numTexCacheRequests++;
}

void
CudaCore::handleTexRetry()
{
    assert(stallOnTexCacheRetry);
    assert(retryTexPkts.size());

    numTexCacheRetry++;

    PacketPtr retry_pkt = retryTexPkts.front();
    DPRINTF(CudaCoreFetch, "Received Tex retry, vaddr: 0x%x\n",
            retry_pkt->req->getVaddr());

    if (texPort.sendTimingReq(retry_pkt)) {
        retryTexPkts.remove(retry_pkt);

        stallOnTexCacheRetry = (retryTexPkts.size() > 0);
        if (stallOnTexCacheRetry) {
            //schedule the rest of waiting requests in the following cycle
            CachePortRetryEvent * e = new CachePortRetryEvent(this, CorePortType::Tex);
            schedule(e, clockEdge(Cycles(1)));
        }
    } else {
        //if the request fails then a retry is expected
        DPRINTF(CudaCoreFetch, "Tex retry, vaddr: 0x%x failed, waiting for another retry\n",
            retry_pkt->req->getVaddr());
    }
}

void
CudaCore::recvTexResp(PacketPtr pkt)
{
    assert(pkt->req->isTexFetch());
    map<Addr,mem_fetch *>::iterator iter =
            texBusyCacheLineAddrs.find(cudaGPU->addrToLine(pkt->req->getVaddr()));
    assert(iter != texBusyCacheLineAddrs.end());

    DPRINTF(CudaCoreFetch, "Finished fetching tex on vaddr 0x%x\n",
            pkt->req->getVaddr());

    shaderImpl->accept_ldst_unit_response(iter->second);
    
    //writing to the corresponding gpgpusim registers 
    //note that if gpgpusim texture cache model is used this might also write
    //to threads that are pending on the same memory block but which have
    //sent a different request. Meaning they will be functionally served 
    //by earlier memory requests from the cache to the same memory block
    assert(shaderImpl->get_gpu()->get_config().get_texcache_linesize() == pkt->getSize());
    uint8_t * data = new uint8_t[pkt->getSize()];
    pkt->writeData(data);
    
    delete [] data;

    texBusyCacheLineAddrs.erase(iter);

    if (pkt->req) delete pkt->req;
    delete pkt;
}

void
CudaCore::texCacheFetch(Addr addr, mem_fetch *mf)
{
    xCacheFetch(addr, mf, "tex", texMasterId, &texBusyCacheLineAddrs,
            ttb, Request::BYPASS_L1, Request::TEX_FETCH);
}

void CudaCore::finishTranslation(WholeTranslationState *state)
{
    if (state->getFault() != NoFault) {
        panic("Core cache translation encountered fault (%s) for address 0x%x",
              state->getFault()->name(), state->mainReq->getVaddr());
    }
    assert(state->mode == BaseTLB::Read);
    PacketPtr pkt = new Packet(state->mainReq, MemCmd::ReadReq);
    pkt->allocate();

    if (pkt->req->isInstFetch()) {
        if (!stallOnICacheRetry) {
            sendInstAccess(pkt);
        } else {
            DPRINTF(CudaCoreFetch, "Inst port blocked, add vaddr: 0x%x to retry list\n",
                    pkt->req->getVaddr());
            retryInstPkts.push_back(pkt);
        }
    } else if (pkt->req->isTexFetch()) {
        if (!stallOnTexCacheRetry) {
            sendTexAccess(pkt);
        } else {
            DPRINTF(CudaCoreFetch, "Texture port blocked, add vaddr: 0x%x to retry list\n",
                    pkt->req->getVaddr());
            retryTexPkts.push_back(pkt);
        }
    } else assert(0);
    delete state;
}

void
CudaCore::xCacheFetch(Addr addr, mem_fetch *mf, const char * type,
        MasterID masterId, std::map<Addr,mem_fetch *> * busyCacheLinesMap
        , ShaderTLB * tlb, Request::FlagsType flagType, Request::FlagsType gpuFlagType)
{
    assert(isCacheResourceAvailable(addr, busyCacheLinesMap));

    Addr line_addr = cudaGPU->addrToLine(addr);
    DPRINTF(CudaCoreFetch,
            "Fetch %s, addr: 0x%x, size: %d, line: 0x%x\n",
            type, addr, mf->get_data_size(), line_addr);

    RequestPtr req = new Request();
    Request::Flags flags;
    Addr pc = (Addr)mf->get_pc();
    const int asid = 0;

    BaseTLB::Mode mode = BaseTLB::Read;
    req->setVirt(asid, line_addr, mf->get_data_size(), flags, masterId, pc);
    req->setFlags(flagType);

    WholeTranslationState *state =
            new WholeTranslationState(req, NULL, NULL, mode);
    DataTranslation<CudaCore*> *translation
            = new DataTranslation<CudaCore*>(this, state);

    (*busyCacheLinesMap)[cudaGPU->addrToLine(req->getVaddr())] = mf;
    tlb->beginTranslateTiming(req, translation, mode);
}

bool
CudaCore::texCacheResAvailabe(Addr a){
    return isCacheResourceAvailable(a,&texBusyCacheLineAddrs);
}
