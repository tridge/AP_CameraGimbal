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
    // Centre payload: sums of regions 5, 6, 9 and 10, saturating per field.
    {
        std::array<uint8_t,448> grid{};
        const size_t offsets[]={0,80,160,240,320,400};
        for(unsigned field=0;field<6;field++) {
            const size_t stride=field==5?3:5;
            for(unsigned region=0;region<16;region++)
                grid[offsets[field]+region*stride]=(uint8_t)(region+field); // low byte only
        }
        uint8_t centre[27];
        assert(ca_zt30_pack_focus_centre(grid.data(),grid.size(),centre,sizeof(centre)));
        const size_t at[]={0,5,10,14,19,24};
        for(unsigned field=0;field<6;field++)assert(centre[at[field]]==5+6+9+10+4*field && centre[at[field]+1]==0);
        // the 32-bit third field saturates when the 40-bit sum does not fit
        for(unsigned region:{5U,6U,9U,10U})memset(grid.data()+160+region*5,0xff,4);
        assert(ca_zt30_pack_focus_centre(grid.data(),grid.size(),centre,sizeof(centre)));
        for(unsigned byte=0;byte<4;byte++)assert(centre[10+byte]==0xff);
        assert(!ca_zt30_pack_focus_centre(grid.data(),447,centre,sizeof(centre)));
        assert(!ca_zt30_pack_focus_centre(grid.data(),448,centre,26));
        // offered statistics are taken once
        uint8_t taken[27];
        assert(!ca_zt30_take_focus(taken));
        assert(ca_zt30_offer_focus(grid.data(),grid.size()) && ca_zt30_take_focus(taken));
        assert(memcmp(taken,centre,27)==0 && !ca_zt30_take_focus(taken));
    }
}
