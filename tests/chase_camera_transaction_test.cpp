#ifdef NDEBUG
#undef NDEBUG
#endif
#include "chase_camera_transaction.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

using namespace grid2vr::chasecamera;
namespace {
constexpr uintptr_t slot=0x12f008,timing=slot+0x294,record=slot+0x18;
const TransactionIdentity owner{0x20000,0x30000,0x40000,0x50000,7,90,4};

bool Begin(CameraTransaction& transaction,const TransactionIdentity& identity=owner) {
    return transaction.Begin(identity,transactionBeginCallerRva,slot,timing);
}
bool Copy(CameraTransaction& transaction,const TransactionIdentity& identity=owner) {
    return transaction.Copy(identity,transactionCopyCallerRva,slot,record);
}
bool Commit(CameraTransaction& transaction,const TransactionIdentity& identity=owner) {
    return transaction.Commit(identity,transactionCommitCallerRva,slot,record);
}

void CompleteAndDuplicate() {
    CameraTransaction transaction;
    assert(!transaction.Active() && transaction.Record()==0);
    assert(Begin(transaction));
    assert(transaction.Active() && transaction.Identity()==owner && transaction.Record()==record);
    assert(Copy(transaction));
    assert(Commit(transaction));
    assert(!transaction.Active() && transaction.Record()==0);
    assert(!Commit(transaction));
    assert(!Begin(transaction)); // Same completed evaluation cannot run twice.
    auto next=owner;
    ++next.evaluation;
    assert(Begin(transaction,next) && Copy(transaction,next) && Commit(transaction,next));
    transaction.Reset();
    assert(Begin(transaction)); // Explicit session reset discards old provenance.
}

void IdentityChanges() {
    for(unsigned field=0;field<7;++field) {
        auto changed=owner;
        switch(field) {
            case 0:changed.camera+=4;break;
            case 1:changed.manager+=4;break;
            case 2:changed.provider+=4;break;
            case 3:changed.entity+=4;break;
            case 4:++changed.thread;break;
            case 5:++changed.evaluation;break;
            case 6:++changed.suspension;break;
        }
        CameraTransaction transaction;
        assert(Begin(transaction));
        assert(!Copy(transaction,changed) && !transaction.Active());
        assert(!Copy(transaction) && !Commit(transaction));
        assert(!Begin(transaction)); // Failed transactions cannot be resurrected.
        transaction.Reset();
        assert(Begin(transaction) && Copy(transaction));
        assert(!Commit(transaction,changed) && !transaction.Active());
    }
    for(unsigned field=0;field<5;++field) {
        auto invalid=owner;
        switch(field) {
            case 0:invalid.camera=0;break;
            case 1:invalid.manager=3;break;
            case 2:invalid.provider=UINT32_MAX;break;
            case 3:invalid.entity=0x50001;break;
            case 4:invalid.thread=0;break;
        }
        CameraTransaction transaction;
        assert(!Begin(transaction,invalid) && !transaction.Active());
    }
}

void CallerAndStack() {
    CameraTransaction transaction;
    assert(!transaction.Begin(owner,transactionBeginCallerRva+1,slot,timing));
    assert(!transaction.Begin(owner,transactionBeginCallerRva,slot+4,timing+4));
    assert(!transaction.Begin(owner,transactionBeginCallerRva,slot,timing+4));
    assert(!transaction.Begin(owner,transactionBeginCallerRva,8,8+0x294));
    assert(!transaction.Begin(owner,transactionBeginCallerRva,0xfffffff8u,0x28c));
    assert(Begin(transaction));
    assert(!transaction.Copy(owner,transactionCopyCallerRva+1,slot,record));
    assert(!Commit(transaction));
    transaction.Reset();
    assert(Begin(transaction));
    assert(!transaction.Copy(owner,transactionCopyCallerRva,slot+16,record+16));
    transaction.Reset();
    assert(Begin(transaction));
    assert(!transaction.Copy(owner,transactionCopyCallerRva,slot,record+16));
    for(unsigned failure=0;failure<3;++failure) {
        transaction.Reset();
        assert(Begin(transaction) && Copy(transaction));
        assert(!transaction.Commit(owner,transactionCommitCallerRva+(failure==0?1:0),
            slot+(failure==1?16:0),record+(failure==2?16:0)));
        assert(!transaction.Active());
    }
}

void SequenceFailures() {
    CameraTransaction transaction;
    assert(!Copy(transaction) && !Commit(transaction));
    assert(Begin(transaction));
    assert(!Commit(transaction)); // Copy must precede native copyback.
    assert(!Copy(transaction));
    transaction.Reset();
    assert(Begin(transaction));
    auto next=owner;
    ++next.evaluation;
    assert(!Begin(transaction,next)); // Nested start invalidates original.
    assert(!Copy(transaction) && !Commit(transaction));
    transaction.Reset();
    assert(Begin(transaction) && Copy(transaction));
    assert(!Copy(transaction)); // A repeated copy cannot commit stale storage.
    assert(!Commit(transaction));
}

// Optional real-image check: rebuild a loaded PE image, apply its actual HIGHLOW
// relocation table, then test native and relocated bases. The fixture reads no
// process and never executes any image instructions.
void VerifyImage(const char* path) {
    std::ifstream file(path,std::ios::binary);
    assert(file.good());
    const std::vector<uint8_t> disk{std::istreambuf_iterator<char>(file),{}};
    const auto word=[&](size_t offset)->uint16_t {
        assert(offset+2<=disk.size());uint16_t value{};
        std::memcpy(&value,disk.data()+offset,2);return value;
    };
    const auto dword=[&](size_t offset)->uint32_t {
        assert(offset+4<=disk.size());uint32_t value{};
        std::memcpy(&value,disk.data()+offset,4);return value;
    };
    const size_t pe=dword(0x3c),optional=pe+24,sections=optional+word(pe+20);
    assert(dword(pe)==0x4550 && word(optional)==0x10b && dword(optional+28)==0x400000);
    std::vector<uint8_t> image(dword(optional+56));
    for(unsigned i=0;i<word(pe+6);++i) {
        const size_t header=sections+i*40;
        const uint32_t rva=dword(header+12),size=dword(header+16),offset=dword(header+20);
        assert(static_cast<size_t>(rva)+size<=image.size() && static_cast<size_t>(offset)+size<=disk.size());
        std::memcpy(image.data()+rva,disk.data()+offset,size);
    }
    const auto imageDword=[&](size_t offset)->uint32_t {
        assert(offset+4<=image.size());uint32_t value{};
        std::memcpy(&value,image.data()+offset,4);return value;
    };
    for(const uintptr_t base:{uintptr_t{0x400000},uintptr_t{0x10000000}}) {
        if(base!=0x400000) {
            const uint32_t start=dword(optional+96+5*8),length=dword(optional+96+5*8+4);
            size_t at=start;
            while(at<static_cast<size_t>(start)+length) {
                const uint32_t page=imageDword(at),block=imageDword(at+4);
                if(!page && !block)break;
                assert(block>=8 && block%2==0 && at+block<=static_cast<size_t>(start)+length);
                for(size_t entry=at+8;entry<at+block;entry+=2) {
                    uint16_t fixup{};std::memcpy(&fixup,image.data()+entry,2);
                    assert(fixup>>12==0 || fixup>>12==3);
                    if(fixup>>12==3) {
                        const size_t address=page+(fixup&0xfff);
                        const uint32_t relocated=imageDword(address)+static_cast<uint32_t>(base)-0x400000u;
                        std::memcpy(image.data()+address,&relocated,4);
                    }
                }
                at+=block;
            }
        }
        size_t largestRead{};
        const auto read=[&](uintptr_t address,void* output,size_t length) {
            largestRead=std::max(largestRead,length);
            if(address<base || address-base>image.size() || length>image.size()-(address-base))return false;
            std::memcpy(output,image.data()+address-base,length);return true;
        };
        assert(TransactionGuardsMatch(base,read) && largestRead<=64);
        for(const size_t at:{size_t{0x70ec26},size_t{0x70ed33},size_t{0x70edb3},size_t{0x70edd7},
                             size_t{0x5afc13},size_t{0x410c6b},size_t{0x1ea43c},size_t{0xd099ac}}) {
            image[at]^=1;
            assert(!TransactionGuardsMatch(base,read));
            image[at]^=1;
        }
        assert(!TransactionGuardsMatch(base,[](uintptr_t,void*,size_t){return false;}));
    }
    std::printf("transaction guards verified: %s\n",path);
}
}

int main(int argc,char** argv) {
    CompleteAndDuplicate();IdentityChanges();CallerAndStack();SequenceFailures();
    for(int i=1;i<argc;++i)VerifyImage(argv[i]);
    std::puts("chase_camera_transaction_test: passed");
}
