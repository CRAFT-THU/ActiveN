funct7 = "0000001"
rs2 = 18
rs1 = 10
funct3 = "101"
rd = 0
opcode = "00010"

opbin = funct7 + "{:05b}".format(rs2) + "{:05b}".format(rs1) + funct3 + "{:05b}".format(rd) + opcode + "11"

print(opbin)
print(int(opbin, 2))

if len(opbin) != 32:
  print("Wrong len: ", len(opbin))
else:
  print("0x" + "{:08x}".format(int(opbin, 2)))
