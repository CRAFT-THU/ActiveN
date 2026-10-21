# Opt-in workload entrypoint; preserve the shared integration-test Makefile.
include Makefile
.DEFAULT_GOAL := selected-workloads

# Selected workload builds. Explicit flags and separate filenames fix identity.
# Historical experiments remain available through generic Makefile switches.
SELECTED_MNIST_SOURCES = Makefile selected.mk sys/spike_count.h sys/mnist_train.S sys/mnist_sparse.inc sys/mnist_sparse_layout.inc sys/mnist_numeric.inc sys/mnist_neuron_pair.inc sys/mnist_fifth_root.inc sys/mnist_fifth_root_config.inc sys/snn_init.c sys/linker_mnist_train.ld
SELECTED_MNIST_FLAGS = -mabi=ilp32 -march=rv32ima_zfinx_zicsr -nostdlib -nostartfiles -Wl,-T,sys/linker_mnist_train.ld -Isys -DMEM_LINE_BYTES=32 -DMNIST_TRAIN=1 -O3 -fomit-frame-pointer -ffp-contract=off -fno-jump-tables
SELECTED_MNIST_COMMON = -DMNIST_RECURRENT_MODE=1 -DMNIST_TRACE_BUNDLE=0 -DMNIST_FIFTH_ROOT_LUT=0

sys/mnist_dense_one-trace.bin: $(SELECTED_MNIST_SOURCES)
	${PREFIX}gcc $(SELECTED_MNIST_FLAGS) -DMNIST_RULE=1 $(SELECTED_MNIST_COMMON) -DMNIST_PARALLEL_INPUT_PREPARE=1 -DMNIST_INPUT_DECAY_MODE=1 -DMNIST_FUSE_NEURON_UPDATE=1 -DMNIST_PIPELINE_INPUT_PREPARE=1 -DMNIST_HORIZONTAL_GROUP_COUNT=8 -o $(@:.bin=.elf) sys/mnist_train.S sys/snn_init.c
	${PREFIX}objcopy --wildcard -j .text -j '.rodata*' -j '.srodata*' -j .data -O binary $(@:.bin=.elf) $@

sys/mnist_sparse_one-trace_mode3.bin: $(SELECTED_MNIST_SOURCES)
	${PREFIX}gcc $(SELECTED_MNIST_FLAGS) -DMNIST_RULE=1 $(SELECTED_MNIST_COMMON) -DMNIST_SPARSE_MODE=3 -DMNIST_SPARSE_FUSE_FORWARD=1 -DMNIST_SPARSE_COMPACT_SCHEDULE=1 -DMNIST_SPARSE_FUSE_NEURON=0 -DMNIST_SPARSE_BINARY_REVERSE=2 -DMNIST_SPARSE_ACTIVE_PREFIX=0 -DMNIST_SPARSE_CACHE_REVERSE=1 -DMNIST_SPARSE_RELEVANT_MASKS=0 -DMNIST_SPARSE_MC_DRAIN=0 -DMNIST_SPARSE_IDLE_GATE=256 -o $(@:.bin=.elf) sys/mnist_train.S sys/snn_init.c
	${PREFIX}objcopy --wildcard -j .text -j '.rodata*' -j '.srodata*' -j .data -O binary $(@:.bin=.elf) $@

sys/mnist_sparse_one-trace_mode4.bin: $(SELECTED_MNIST_SOURCES)
	${PREFIX}gcc $(SELECTED_MNIST_FLAGS) -DMNIST_RULE=1 $(SELECTED_MNIST_COMMON) -DMNIST_SPARSE_MODE=4 -DMNIST_SPARSE_FUSE_FORWARD=1 -DMNIST_SPARSE_COMPACT_SCHEDULE=1 -DMNIST_SPARSE_FUSE_NEURON=0 -DMNIST_SPARSE_BINARY_REVERSE=2 -DMNIST_SPARSE_ACTIVE_PREFIX=0 -DMNIST_SPARSE_CACHE_REVERSE=1 -DMNIST_SPARSE_RELEVANT_MASKS=0 -DMNIST_SPARSE_MC_DRAIN=0 -DMNIST_SPARSE_IDLE_GATE=256 -o $(@:.bin=.elf) sys/mnist_train.S sys/snn_init.c
	${PREFIX}objcopy --wildcard -j .text -j '.rodata*' -j '.srodata*' -j .data -O binary $(@:.bin=.elf) $@

sys/mnist_dense_two-trace.bin: $(SELECTED_MNIST_SOURCES)
	${PREFIX}gcc $(SELECTED_MNIST_FLAGS) -DMNIST_RULE=2 $(SELECTED_MNIST_COMMON) -DMNIST_PARALLEL_INPUT_PREPARE=1 -DMNIST_INPUT_DECAY_MODE=1 -DMNIST_FUSE_NEURON_UPDATE=1 -DMNIST_PIPELINE_INPUT_PREPARE=1 -DMNIST_HORIZONTAL_GROUP_COUNT=8 -o $(@:.bin=.elf) sys/mnist_train.S sys/snn_init.c
	${PREFIX}objcopy --wildcard -j .text -j '.rodata*' -j '.srodata*' -j .data -O binary $(@:.bin=.elf) $@

sys/mnist_sparse_two-trace_mode3.bin: $(SELECTED_MNIST_SOURCES)
	${PREFIX}gcc $(SELECTED_MNIST_FLAGS) -DMNIST_RULE=2 $(SELECTED_MNIST_COMMON) -DMNIST_SPARSE_MODE=3 -DMNIST_SPARSE_FUSE_FORWARD=1 -DMNIST_SPARSE_COMPACT_SCHEDULE=1 -DMNIST_SPARSE_FUSE_NEURON=1 -DMNIST_SPARSE_BINARY_REVERSE=2 -DMNIST_SPARSE_ACTIVE_PREFIX=0 -DMNIST_SPARSE_CACHE_REVERSE=1 -DMNIST_SPARSE_RELEVANT_MASKS=0 -DMNIST_SPARSE_MC_DRAIN=0 -DMNIST_SPARSE_IDLE_GATE=256 -o $(@:.bin=.elf) sys/mnist_train.S sys/snn_init.c
	${PREFIX}objcopy --wildcard -j .text -j '.rodata*' -j '.srodata*' -j .data -O binary $(@:.bin=.elf) $@

sys/mnist_sparse_two-trace_mode4.bin: $(SELECTED_MNIST_SOURCES)
	${PREFIX}gcc $(SELECTED_MNIST_FLAGS) -DMNIST_RULE=2 $(SELECTED_MNIST_COMMON) -DMNIST_SPARSE_MODE=4 -DMNIST_SPARSE_FUSE_FORWARD=1 -DMNIST_SPARSE_COMPACT_SCHEDULE=1 -DMNIST_SPARSE_FUSE_NEURON=1 -DMNIST_SPARSE_BINARY_REVERSE=2 -DMNIST_SPARSE_ACTIVE_PREFIX=0 -DMNIST_SPARSE_CACHE_REVERSE=1 -DMNIST_SPARSE_RELEVANT_MASKS=0 -DMNIST_SPARSE_MC_DRAIN=0 -DMNIST_SPARSE_IDLE_GATE=256 -o $(@:.bin=.elf) sys/mnist_train.S sys/snn_init.c
	${PREFIX}objcopy --wildcard -j .text -j '.rodata*' -j '.srodata*' -j .data -O binary $(@:.bin=.elf) $@

sys/mnist_dense_three-trace.bin: $(SELECTED_MNIST_SOURCES)
	${PREFIX}gcc $(SELECTED_MNIST_FLAGS) -DMNIST_RULE=3 $(SELECTED_MNIST_COMMON) -DMNIST_PARALLEL_INPUT_PREPARE=1 -DMNIST_INPUT_DECAY_MODE=1 -DMNIST_FUSE_NEURON_UPDATE=1 -DMNIST_PIPELINE_INPUT_PREPARE=1 -DMNIST_HORIZONTAL_GROUP_COUNT=8 -o $(@:.bin=.elf) sys/mnist_train.S sys/snn_init.c
	${PREFIX}objcopy --wildcard -j .text -j '.rodata*' -j '.srodata*' -j .data -O binary $(@:.bin=.elf) $@

sys/mnist_sparse_three-trace_mode3.bin: $(SELECTED_MNIST_SOURCES)
	${PREFIX}gcc $(SELECTED_MNIST_FLAGS) -DMNIST_RULE=3 $(SELECTED_MNIST_COMMON) -DMNIST_SPARSE_MODE=3 -DMNIST_SPARSE_FUSE_FORWARD=1 -DMNIST_SPARSE_COMPACT_SCHEDULE=1 -DMNIST_SPARSE_FUSE_NEURON=1 -DMNIST_SPARSE_BINARY_REVERSE=2 -DMNIST_SPARSE_ACTIVE_PREFIX=0 -DMNIST_SPARSE_CACHE_REVERSE=1 -DMNIST_SPARSE_RELEVANT_MASKS=0 -DMNIST_SPARSE_MC_DRAIN=0 -DMNIST_SPARSE_IDLE_GATE=256 -o $(@:.bin=.elf) sys/mnist_train.S sys/snn_init.c
	${PREFIX}objcopy --wildcard -j .text -j '.rodata*' -j '.srodata*' -j .data -O binary $(@:.bin=.elf) $@

sys/mnist_sparse_three-trace_mode4.bin: $(SELECTED_MNIST_SOURCES)
	${PREFIX}gcc $(SELECTED_MNIST_FLAGS) -DMNIST_RULE=3 $(SELECTED_MNIST_COMMON) -DMNIST_SPARSE_MODE=4 -DMNIST_SPARSE_FUSE_FORWARD=1 -DMNIST_SPARSE_COMPACT_SCHEDULE=1 -DMNIST_SPARSE_FUSE_NEURON=1 -DMNIST_SPARSE_BINARY_REVERSE=2 -DMNIST_SPARSE_ACTIVE_PREFIX=0 -DMNIST_SPARSE_CACHE_REVERSE=1 -DMNIST_SPARSE_RELEVANT_MASKS=0 -DMNIST_SPARSE_MC_DRAIN=0 -DMNIST_SPARSE_IDLE_GATE=256 -o $(@:.bin=.elf) sys/mnist_train.S sys/snn_init.c
	${PREFIX}objcopy --wildcard -j .text -j '.rodata*' -j '.srodata*' -j .data -O binary $(@:.bin=.elf) $@

SELECTED_WORKLOAD_BINS = sys/mnist_dense_one-trace.bin sys/mnist_sparse_one-trace_mode3.bin sys/mnist_sparse_one-trace_mode4.bin sys/mnist_dense_two-trace.bin sys/mnist_sparse_two-trace_mode3.bin sys/mnist_sparse_two-trace_mode4.bin sys/mnist_dense_three-trace.bin sys/mnist_sparse_three-trace_mode3.bin sys/mnist_sparse_three-trace_mode4.bin sys/brunel_additive_proxy_home_g4.bin sys/brunel_morrison_proxy_home_g4.bin sys/morrison_bulk_worker_scheduled_smt1.bin
.PHONY: selected-workloads
selected-workloads: $(SELECTED_WORKLOAD_BINS)
