#include "camera_app/src/backends/zt30/focus.h"
#include <assert.h>
#include <array>
int main()
{
    std::array<uint8_t,448> block{};
    const uint8_t values[][5]={{1,2,3,4,5},{6,7,8,9,10},{11,12,13,14,255},{15,16,17,18,19},{20,21,22,23,24},{25,26,27,0,0}};
    const uint8_t expected[]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27};
    uint8_t output[29];memset(output,0xaa,sizeof(output));
    // Last region reaches the end of the callback block; third high byte differs.
    for(unsigned i=0;i<5;i++)memcpy(block.data()+i*80+75,values[i],5);
    memcpy(block.data()+445,values[5],3);
    assert(ca_zt30_pack_focus_callback(block.data(),block.size(),15,output+1,27));
    assert(memcmp(output+1,expected,27)==0 && output[0]==0xaa && output[28]==0xaa);
    // Short view, bad index or output capacity write nothing.
    for(size_t size:{size_t(0),size_t(447)}) {
        memset(output,0xaa,sizeof(output));
        assert(!ca_zt30_pack_focus_callback(block.data(),size,15,output,27));
        for(uint8_t b:output)assert(b==0xaa);
    }
    assert(!ca_zt30_pack_focus_callback(block.data(),448,16,output,27));
    assert(!ca_zt30_pack_focus_callback(block.data(),448,0,output,26));
    assert(!ca_zt30_pack_focus_callback(nullptr,448,0,output,27));
    assert(!ca_zt30_pack_focus_callback(block.data(),448,0,nullptr,27));
    // Independent typed source layout exercises every region and every field.
    struct Input { uint8_t first[16][5],second[16][5],third[16][5],fourth[16][5],fifth[16][5],last[16][3]; } input{};
    static_assert(sizeof(input)==448,"vendor callback field view");
    for(unsigned region=0;region<16;region++) {
        for(unsigned byte=0;byte<5;byte++) {
            input.first[region][byte]=(uint8_t)(region*5+byte);
            input.second[region][byte]=(uint8_t)(81+region*3+byte);
            input.third[region][byte]=(uint8_t)(151+region*2+byte);
            input.fourth[region][byte]=(uint8_t)(211-region*4+byte);
            input.fifth[region][byte]=(uint8_t)(29+region*7+byte);
        }
        for(unsigned byte=0;byte<3;byte++)input.last[region][byte]=(uint8_t)(197-region*9+byte);
    }
    for(unsigned region=0;region<16;region++) {
        uint8_t reference[27];
        memcpy(reference,input.first[region],5);memcpy(reference+5,input.second[region],5);
        memcpy(reference+10,input.third[region],4);memcpy(reference+14,input.fourth[region],5);
        memcpy(reference+19,input.fifth[region],5);memcpy(reference+24,input.last[region],3);
        assert(ca_zt30_pack_focus_callback(reinterpret_cast<const uint8_t*>(&input),sizeof(input),region,output,27));
        assert(memcmp(output,reference,27)==0);
    }
    // A callback region must select its own fields, not always use region0.
    assert(ca_zt30_pack_focus_callback(block.data(),448,0,output,27));
    for(unsigned i=0;i<27;i++)assert(output[i]==0);
}
