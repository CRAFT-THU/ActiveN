/* Numeric constants and message instructions shared by sparse kernels. */
#define F_ONE                       0x3f800000
#define F_TWO23                     0x4b000000
#define F_PRE_DECAY                 0x3f7cd1e6
#define F_POST1_DECAY               0x3f7cd1e6
#define F_POST2_DECAY               0x3f7e67ad
#define F_GE_HALF_DECAY             0x3f475f7d
#define F_GI_HALF_DECAY             0x3f61eb51
#define F_GE_DECAY                  0x3f1b4598
#define F_GI_DECAY                  0x3f475f7d
#define F_THETA_DECAY               0x3f7fffff
#define F_E_TO_I                    0x41266666
#define F_I_TO_E                    0x41cc0000
#define F_EXC_REST                  0xc2820000
#define F_INH_REST                  0xc2700000
#define F_EXC_INH_REV               0xc2c80000
#define F_INH_INH_REV               0xc2aa0000
#define F_EXC_THRESHOLD_OFFSET      0x42900000
#define F_INH_THRESHOLD             0xc2200000
#define F_EXC_RESET                 0xc2820000
#define F_INH_RESET                 0xc2340000
#define F_EXC_DT                    0x3ba3d70a
#define F_INH_DT                    0x3d4ccccd
#define F_THETA_INCREMENT           0x3d4ccccd

  .macro SEND dst_reg, tag_reg
  .insn r 0x0B, 0x4, 0x0, x0, \dst_reg, \tag_reg
  .endm

  .macro YIELD dst_reg, tag_reg
  .insn r 0x0B, 0x5, 0x0, x0, \dst_reg, \tag_reg
  .endm

  .macro SENDI dst_reg, tag_imm
  .insn i 0x2B, 0x4, x0, \dst_reg, \tag_imm
  .endm

  .macro YIELDI dst_reg, tag_imm
  .insn i 0x2B, 0x5, x0, \dst_reg, \tag_imm
  .endm

