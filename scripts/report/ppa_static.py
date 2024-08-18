import sys

# Static PPA metrics

# Logic area & power
# These data are directly given by EDA tools using 28nm process node
CORE_AREA_FPU = 16181.927827
CORE_AREA_FIXED_AUX = 7919.015915
CORE_AREA_PIPELINE_BASELINE = 45338.4318
CORE_AREA_PIPELINE_ENHANCED = 52108.727832

CORE_POWER_FPU = 5.333
CORE_POWER_FIXED_AUX = 6.895
CORE_POWER_PIPELINE_BASELINE = 33.278
CORE_POWER_PIPELINE_ENHANCED = 39.443

# Memories
# These data are generated with CACTI7.0
MEMORY_AREA_QUEUE = 16058
MEMORY_AREA_ICACHE = 5753
MEMORY_AREA_SP = 94187

MEMORY_POWER_QUEUE = 4.13704
MEMORY_POWER_ICACHE = 0.874
MEMORY_POWER_SP = 4.012

# NoC
# These data are generated with ORION 3
# Power & area for NoC in each cluster is divided into the IO router and the internal network
NOC_POWER_CLUSTER_ROUTER = 2.17967
NOC_POWER_CLUSTER_NETWORK = 62.7911

NOC_AREA_CLUSTER_ROUTER = 10922.2
NOC_AREA_CLUSTER_NETWORK = 116873

def core_area(cfg):
  if cfg == "fixed":
    return CORE_AREA_FIXED_AUX + CORE_AREA_PIPELINE_BASELINE + MEMORY_AREA_QUEUE
  elif cfg == "baseline":
    return CORE_AREA_FIXED_AUX + CORE_AREA_PIPELINE_BASELINE + MEMORY_AREA_QUEUE + CORE_AREA_FPU
  elif cfg == "enhanced":
    return CORE_AREA_FIXED_AUX + CORE_AREA_PIPELINE_ENHANCED + MEMORY_AREA_QUEUE + CORE_AREA_FPU

def core_power(cfg):
  if cfg == "fixed":
    return CORE_POWER_FIXED_AUX + CORE_POWER_PIPELINE_BASELINE + MEMORY_POWER_QUEUE
  elif cfg == "baseline":
    return CORE_POWER_FIXED_AUX + CORE_POWER_PIPELINE_BASELINE + MEMORY_POWER_QUEUE + CORE_POWER_FPU
  elif cfg == "enhanced":
    return CORE_POWER_FIXED_AUX + CORE_POWER_PIPELINE_ENHANCED + MEMORY_POWER_QUEUE + CORE_POWER_FPU

# MVC runtime
print("MVC runtime power\n====")
static_peak = 32 * ((core_power("enhanced") + MEMORY_POWER_ICACHE + MEMORY_POWER_SP) * 16 + NOC_POWER_CLUSTER_ROUTER + NOC_POWER_CLUSTER_NETWORK)
print(f"  Static maximum power: {static_peak / 1000:.3f} W")
print(f"  Dynamic DRAM power: {float(sys.argv[1]) / 1000:.3f} W")
print(f"  Total maximum power: {static_peak / 1000 + float(sys.argv[1]) / 1000:.3f} W")

print()

# Whole system settings
print("Whole system area / peak power\n====")
for cfg in ["fixed", "baseline", "enhanced"]:
  print(f"Config: {cfg}")
  print(f"  Core\t\t area {core_area(cfg):.3f} um^2, power {core_power(cfg):.3f} mW")
  pu_area = core_area(cfg) + MEMORY_AREA_ICACHE + MEMORY_AREA_SP
  pu_power = core_power(cfg) + MEMORY_POWER_ICACHE + MEMORY_POWER_SP
  print(f"  PU\t\t area {pu_area:.3f} um^2, power {pu_power:.3f} mW")
  cluster_area = pu_area * 16 + NOC_AREA_CLUSTER_ROUTER + NOC_AREA_CLUSTER_NETWORK
  cluster_power = pu_power * 16 + NOC_POWER_CLUSTER_ROUTER + NOC_POWER_CLUSTER_NETWORK
  print(f"  Cluster\t area {cluster_area / 1000000:.3f} mm^2, power {cluster_power / 1000:.3f} W")
  print(f"  System\t area {cluster_area * 32 / 1000000:.3f} mm^2, power {cluster_power * 32 / 1000:.3f} W")
