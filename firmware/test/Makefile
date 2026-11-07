# Host tests for the Muster runtime. Run from repo root: make -C firmware/test
CC      ?= gcc
CXX     ?= g++
AVRCC   ?= avr-gcc
AVRSIZE ?= avr-size
SRC      = ../src
CFLAGS   = -std=c99 -Wall -Wextra -Wpedantic -Werror -O2 -I$(SRC)
CXXFLAGS = -std=c++11 -Wall -Wextra -Werror -O2 -I$(SRC)
HDRS     = $(wildcard $(SRC)/*.h) rig.h
TESTS    = test_bus test_core test_framing test_loss test_modules test_multilink test_sim_cluster test_uart_link test_espnow_link test_trio_task test_spi_frame test_spi_link
AVRFLAGS = -mmcu=atmega328p -std=c99 -Os -Wall -Wextra -Werror -I$(SRC) \
           -ffunction-sections -fdata-sections

.PHONY: test cpp avr clean
test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

test_%: test_%.c $(HDRS)
	$(CC) $(CFLAGS) $< -lm -o $@

# the cluster-in-a-chip logic lives with its example
test_sim_cluster: ../examples/sim_cluster/src/cluster.h
test_trio_task: ../examples/trio/muster_trio/trio_task.h

# Same tests compiled as C++11, because Arduino compiles sketches as C++.
cpp: $(HDRS)
	@for t in $(TESTS); do $(CXX) $(CXXFLAGS) -x c++ $$t.c -o $$t.cpp.out && ./$$t.cpp.out || exit 1; done

# ATmega328P footprint of a minimal worker. Needs avr-gcc + avr-size.
# Static RAM = .data + .bss. Stack frames are from the same -Os build.
avr: avr_worker.c $(HDRS)
	$(AVRCC) $(AVRFLAGS) -Wl,--gc-sections avr_worker.c -o avr_worker.elf
	@$(AVRSIZE) -A avr_worker.elf | awk '/^\.text|^\.data|^\.bss/ {s[$$1]=$$2} \
	  END {printf "flash %d B (.text %d + .data %d)\nstatic RAM %d B (.data %d + .bss %d)\n", \
	  s[".text"]+s[".data"], s[".text"], s[".data"], s[".data"]+s[".bss"], s[".data"], s[".bss"]}'
	$(AVRCC) $(AVRFLAGS) -fstack-usage -c avr_worker.c -o avr_worker.o
	@echo "largest stack frames (bytes):"; sort -t'	' -k2 -nr avr_worker.su | head -6

clean:
	rm -f $(TESTS) *.cpp.out *.elf *.su *.o
