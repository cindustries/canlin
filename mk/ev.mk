# Event scheduler core (src/ev). Portable C99, no dependencies.
EV_HDR := include/canlin/ev.h include/canlin/ev_limits.h
EV_OBJ := $(BUILD)/src/ev/ev.o

LIBOBJ += $(EV_OBJ)
TESTS += $(BUILD)/t/ev/test_ev

$(EV_OBJ) $(BUILD)/t/ev/test_ev.o: $(EV_HDR)

$(BUILD)/t/ev/test_ev: $(BUILD)/t/ev/test_ev.o $(EV_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@
