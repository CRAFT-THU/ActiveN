#include <verilated_dpi.h>
#include "verilated/rtl.h"
#include "verilated/rtl__Dpi.h"
#include <iostream>

#include "meow.h"

extern bool LOG;

extern "C" {
long long softfpu_compute(const svLogicVecVal *a, const svLogicVecVal *b, const svLogicVecVal *funct7, const svLogicVecVal *funct3, svLogic rs2b0) {
  float afv = *reinterpret_cast<const float *>(&a->aval);
  float bfv = *reinterpret_cast<const float *>(&b->aval);

  // if(LOG) std::cout<<"[Meow] Computing: "<<afv<<" "<<bfv<<std::endl;

  uint32_t aiv = a->aval;
  uint32_t biv = b->aval;

  uint32_t f7 = funct7->aval;
  uint32_t f3 = funct3->aval;

  float result;

  if(f7 == 0b0000000) { // FADD
    result  = afv + bfv;
  } else if(f7 == 0b0000100) { // FSUB
    result = afv - bfv;
  } else if(f7 == 0b0001000) { // FMUL
    result = afv * bfv;
  } else if(f7 == 0b0010000) { // SIGN transfer
    uint32_t asign = aiv & 0x80000000;
    uint32_t bsign = biv & 0x80000000;
    uint32_t arest = aiv & 0x7FFFFFFF;

    uint32_t iresult;

    if(f3 == 0b000) iresult = bsign | arest;
    else if(f3 == 0b001) iresult = (bsign | arest) ^0x80000000;
    else if(f3 == 0b010) iresult = (bsign ^ asign) | arest;

    *reinterpret_cast<uint32_t *>(&result) = iresult;
  } else if(f7 == 0b0010100) { // MinMax
    if(f3 == 0b000) result = std::min(afv, bfv);
    else if(f3 == 0b001) result = std::max(afv, bfv);
  } else if(f7 == 0b1100000) { // FCVT.W[U].S
    if(rs2b0) *reinterpret_cast<uint32_t *>(&result) = aiv;
    else *reinterpret_cast<int32_t *>(&result) = aiv;
  } else if(f7 == 0b1010000) { // cmp
    bool bresult;
    if(f3 == 0b010) bresult = afv == bfv;
    else if(f3 == 0b001) bresult = afv < bfv;
    else if(f3 == 0b000) bresult = afv <= bfv;

    *reinterpret_cast<uint32_t *>(&result) = bresult ? 1 : 0;
  } else if(f7 == 0b1101000) { // FCVT.S.W[U]
    if(rs2b0) {
      result = aiv;
    } else {
      int32_t aiiv = *reinterpret_cast<int32_t *>(&aiv);
      result = aiiv;
    }
  } else {
    *reinterpret_cast<uint32_t *>(&result) = 0xDEADBEEF;
  }

  return *reinterpret_cast<int32_t *>(&result);
}

int softfpu_delay(const svLogicVecVal *funct7, const svLogicVecVal *funct3) {
  return 3;
}

long long softmem_read(const svLogicVecVal* hartid, const svLogicVecVal* addr) {
  return meow_softmem_read(hartid->aval, addr->aval);
}
// DPI import at /root/workspace/CRAFT/Koneko/sim/../Aux.sv:4:30
void softmem_write(const svLogicVecVal* hartid, const svLogicVecVal* addr, const svLogicVecVal* wdata, const svLogicVecVal* we) {
  meow_softmem_write(hartid->aval, addr->aval, wdata->aval, we->aval);
}
}
