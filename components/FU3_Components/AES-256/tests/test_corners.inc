// Methods of Bench; included within the test fixture to share its MMIO driver.
static Words words(const Block& block) {
    Words out{};
    for (unsigned i = 0; i < 16; ++i) out[i / 4] |= std::uint32_t(block[i]) << (24 - 8 * (i % 4));
    return out;
}
void oracle(bool wide) {
    const auto all = load_vectors(vector_path);
    unsigned count = 0;
    for (const auto& v : all) {
        if (v.wide != wide) continue;
        wr(reg::CONFIG, wide ? 3 : 1);
        for (unsigned i = 0; i < 8; ++i) {
            std::uint32_t word = 0;
            for (unsigned j = 0; j < 4; ++j) word = (word << 8) | v.key[4*i+j];
            wr(reg::key(i), word);
        }
        input(words(v.input)); init(); next();
        try {
            output(words(v.encrypted));
            wr(reg::CONFIG, wide ? 2 : 0);
            // Decrypt the independent input, not the model's own ciphertext.
            next(); output(words(v.decrypted));
        } catch (const std::exception& e) {
            throw std::runtime_error("oracle vector " + std::to_string(count) + ": " + e.what());
        }
        ++count;
    }
    check(count == (wide ? 643u : 515u), "oracle corpus count");
    std::cout << "MMIO independent vectors=" << count << " operations=" << 2 * count << '\n';
}
void strobes() {
    for (unsigned bits = 0; bits < 16; ++bits) {
        program(true); input(plain);
        unsigned char be[4];
        std::uint32_t mask = 0;
        for (unsigned j = 0; j < 4; ++j) {
            be[j] = (bits & (1u << j)) ? 0xff : 0;
            if (be[j]) mask |= 0xffu << (8*j);
        }
        // A masked write restores exactly the enabled lanes of every key/input word.
        for (unsigned i = 0; i < 12; ++i) {
            const auto off = i < 8 ? reg::key(i) : reg::block(i - 8);
            const auto wanted = i < 8 ? key_words[i] : plain[i - 8];
            wr(off, wanted ^ mask);
            auto value = wanted;
            check(access(true, off, value, be, 4) == tlm::TLM_OK_RESPONSE, "all 16 write strobe masks");
        }
        init(); next(); output(cipher256);
        std::uint32_t value = 0xdeadbeef;
        check(access(false, reg::NAME0, value, be, 4) == tlm::TLM_OK_RESPONSE, "all 16 read masks");
        check(value == ((0xdeadbeefu & ~mask) | (0x61657320u & mask)), "read mask lane merge");
        value = 0;
        check(access(true, reg::CONFIG, value, be, 4) == tlm::TLM_OK_RESPONSE, "strobed CONFIG");
        check(rd(reg::CTRL) == ((bits & 1) ? 0u : 12u), "only CONFIG low lane affects fields");
    }
    // Repeating enables of lengths 1, 2 and 3 use payload-relative lane indices.
    for (unsigned length : {1u, 2u, 3u}) {
        program(true); input(plain);
        unsigned char be[3]{0xff, 0, 0xff};
        std::uint32_t mask = 0;
        for (unsigned j = 0; j < 4; ++j) if (be[j % length]) mask |= 0xffu << (8*j);
        wr(reg::KEY0, key_words[0] ^ mask);
        auto value = key_words[0];
        check(access(true, reg::KEY0, value, be, length) == tlm::TLM_OK_RESPONSE, "repeated strobe merge");
        init(); next(); output(cipher256);
    }
    // A disabled low command lane must not issue NEXT or alter the schedule.
    unsigned char high_only[4]{0, 0xff, 0xff, 0xff};
    std::uint32_t value = 0xffffffff;
    check(access(true, reg::CTRL, value, high_only, 4) == tlm::TLM_OK_RESPONSE, "masked command accepted");
    check(peek(reg::STATUS) == 3 && peek(reg::CTRL) == 12, "masked command has no side effects");
}
void status_flow() {
    program(true); input(plain); init(); next(); output(cipher256);
    wr(reg::CONFIG, 2); input(cipher256);
    check(peek(reg::STATUS) == 3, "input/config writes retain last VALID");
    wr(reg::CTRL, reg::NEXT);
    check(peek(reg::STATUS) == 0, "new job invalidates old result");
    for (unsigned i = 0; i < 4; ++i) check(peek(reg::result(i)) == cipher256[i], "old result held atomically");
    wr(reg::CTRL, 0); wr(reg::CTRL, 0xfffffffcu);
    wr(reg::STATUS, 3); wr(reg::RESULT0, 0); wr(0x3fc, 0xffffffff);
    check(peek(reg::STATUS) == 0, "zero/reserved/RO writes do not finish job");
    unsigned char disabled = 0;
    std::uint32_t value = 0xffffffff;
    for (auto off : {reg::KEY0, reg::BLOCK0, reg::CONFIG, reg::CTRL})
        check(access(true, off, value, &disabled, 1) == tlm::TLM_OK_RESPONSE, "disabled busy write no-op");
    poll(reg::VALID); output(plain);
    wr(reg::CTRL, reg::INIT);
    check(peek(reg::STATUS) == 0, "re-INIT clears VALID");
    poll(reg::READY); check(peek(reg::STATUS) == 1, "INIT does not produce a result");
    next(); output(plain);
    // Reset must clear both hidden WO inputs and the expanded schedule.
    reset_request.notify(SC_ZERO_TIME); wait(reset_done);
    wr(reg::CONFIG, 1); init(); next();
    output(Words{0x66e94bd4, 0xef8a2c3b, 0x884cfa59, 0xca342b2e});
}
void access_matrix() {
    program(true); input(plain); init();
    unsigned char bytes[16]{}, be[4]{0xff, 0xff, 0xff, 0xff};
    tlm::tlm_generic_payload tx;
    tx.set_data_ptr(bytes); tx.set_address(reg::CONFIG);
    tx.set_data_length(4); tx.set_streaming_width(4);
    const auto expect = [&](tlm::tlm_response_status status) {
        const auto t = sc_time_stamp();
        sc_time delay = SC_ZERO_TIME;
        transfer(tx, delay);
        check(tx.get_response_status() == status, "error matrix response");
        check(sc_time_stamp() == t, "malformed transaction must not wait");
        check(peek(reg::CTRL) == 12, "malformed access did not mutate config");
        check(cpu->transport_dbg(tx) == 0, "malformed debug access");
    };
    for (auto cmd : {tlm::TLM_READ_COMMAND, tlm::TLM_WRITE_COMMAND}) {
        tx.set_command(cmd);
        for (unsigned length : {0u, 1u, 2u, 3u, 5u, 8u, 16u}) {
            tx.set_data_length(length); expect(tlm::TLM_BURST_ERROR_RESPONSE);
        }
        tx.set_data_length(4);
        for (unsigned stream = 0; stream < 4; ++stream) {
            tx.set_streaming_width(stream); expect(tlm::TLM_BURST_ERROR_RESPONSE);
        }
        tx.set_streaming_width(4);
        for (std::uint64_t address : {1ULL, 2ULL, 3ULL, 0x3ffULL, 0x400ULL, 0x10000ULL,
                                     0x100000000ULL, 0xffffffffffffffffULL}) {
            tx.set_address(address); expect(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        }
        tx.set_address(reg::CONFIG); tx.set_byte_enable_ptr(be); tx.set_byte_enable_length(4);
        for (unsigned lane = 0; lane < 4; ++lane) {
            for (unsigned invalid : {1u, 0x7fu, 0xfeu}) {
                be[lane] = static_cast<unsigned char>(invalid); expect(tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE);
            }
            be[lane] = 0xff;
        }
        tx.set_byte_enable_ptr(nullptr);
        tx.set_data_ptr(nullptr); expect(tlm::TLM_GENERIC_ERROR_RESPONSE); tx.set_data_ptr(bytes);
    }
    // Larger streaming width still describes one four-byte transfer.
    tx.set_command(tlm::TLM_READ_COMMAND); tx.set_address(reg::NAME0); tx.set_streaming_width(16);
    sc_time delay = SC_ZERO_TIME; transfer(tx, delay);
    check(tx.is_response_ok() && bytes[0] == 0x20 && bytes[3] == 0x61, "wide streaming window");
    tlm::tlm_dmi dmi;
    check(!cpu->get_direct_mem_ptr(tx, dmi), "DMI explicitly unavailable");
    next(); output(cipher256); // Rejected writes must not invalidate or corrupt key/input.
}
void launch_workers() {
    for (unsigned i = 0; i < 3; ++i) {
        workers[i].offset = reg::key(i); workers[i].value = key_words[i];
        workers[i].done = false; workers[i].error.clear();
        workers[i].start.notify(SC_ZERO_TIME);
    }
}
void join_workers(tlm::tlm_response_status expected) {
    while (!workers[0].done || !workers[1].done || !workers[2].done) wait(workers_done);
    for (auto& w : workers) {
        check(w.error.empty(), "worker exception");
        check(w.response == expected, "concurrent response");
    }
}
void concurrency() {
    program(true); input(plain);
    launch_workers(); join_workers(tlm::TLM_OK_RESPONSE);
    check(workers[0].finish != workers[1].finish && workers[0].finish != workers[2].finish &&
          workers[1].finish != workers[2].finish, "concurrent accesses serialized across edges");
    init(); next(); output(cipher256);
    clock_enabled = false;
    launch_workers(); wait(1, SC_NS);
    check(!workers[0].done && !workers[1].done && !workers[2].done, "clock/lock callers are pending");
    reset_request.notify(SC_ZERO_TIME); wait(1, SC_NS);
    clock_enabled = true;
    join_workers(tlm::TLM_GENERIC_ERROR_RESPONSE);
    wait(reset_done);
    check(rd(reg::CTRL) == 0 && rd(reg::STATUS) == 1, "queued calls cancelled by sampled reset");
    // Prove cancelled queued writes did not reappear in hidden key storage.
    wr(reg::CONFIG, 1); init(); next();
    output(Words{0x66e94bd4, 0xef8a2c3b, 0x884cfa59, 0xca342b2e});
    vectors(true);
}
void reset_edges() {
    program(true); input(plain); init(); next(); output(cipher256);
    clock_enabled = false;
    reset_request.notify(SC_ZERO_TIME); wait(1, SC_NS);
    check(peek(reg::STATUS) == 3, "synchronous reset must await edge");
    std::uint32_t value = 0;
    check(access(false, reg::STATUS, value) == tlm::TLM_GENERIC_ERROR_RESPONSE, "bus denied during reset");
    wait(reset_done);
    check(peek(reg::STATUS) == 3, "unsampled reset pulse preserves state");
    clock_enabled = true; output(cipher256);
    reset_request.notify(SC_ZERO_TIME); wait(reset_done);
    check(rd(reg::STATUS) == 1, "idle reset clears VALID");
    for (unsigned i = 0; i < 4; ++i) check(rd(reg::result(i)) == 0, "idle reset clears results");
    // Request entering during reset must not be resurrected by its annotated delay.
    reset_request.notify(SC_ZERO_TIME); wait(1, SC_NS);
    value = 3;
    check(access(true, reg::CONFIG, value, nullptr, 0, sc_time(40, SC_NS)) == tlm::TLM_GENERIC_ERROR_RESPONSE,
          "delayed request issued in reset rejected");
    check(rd(reg::CTRL) == 0, "no delayed write after reset release");
}
void timing_parameters() {
    const auto cfg = config(scenario);
    program(true); input(plain);
    for (unsigned command : {reg::INIT, reg::NEXT}) {
        wr(reg::CTRL, command);
        check(peek(reg::STATUS) == 0, "parameterized job starts busy");
        const auto duration = period * (command == reg::INIT ? cfg.init_cycles : cfg.block_cycles);
        wait(duration - sc_time(1, SC_PS));
        check(peek(reg::STATUS) == 0, "parameterized job not early");
        wait(2, SC_PS);
        check(peek(reg::STATUS) == (command == reg::INIT ? 1u : 3u), "parameterized completion edge");
    }
    output(cipher256);
}
void reset_entry() {
    program(true); input(plain); init(); next(); output(cipher256);
    // Pulse entirely between clock edges: core state must survive, but an MMIO
    // request issued while reset is low must not be accepted after its delay.
    reset_hold = sc_time(1, SC_NS);
    reset_request.notify(SC_ZERO_TIME); wait(1, SC_PS);
    check(!rst.read(), "short reset pulse asserted");
    std::uint32_t value = 0;
    check(access(true, reg::CONFIG, value, nullptr, 0, sc_time(40, SC_NS)) == tlm::TLM_GENERIC_ERROR_RESPONSE,
          "request entering reset cannot revive after annotation delay");
    check(rd(reg::CTRL) == 12, "short reset request did not mutate CONFIG");
    output(cipher256);
}
void busy_matrix() {
    program(true); input(plain);
    for (unsigned command : {reg::INIT, reg::NEXT}) {
        wr(reg::CTRL, command);
        check(peek(reg::CTRL) == (12 | command), "accepted command readback pulse");
        for (unsigned i = 0; i < 8; ++i) reject(reg::key(i), ~key_words[i]);
        for (unsigned i = 0; i < 4; ++i) reject(reg::block(i), ~plain[i]);
        for (unsigned cfg = 0; cfg < 4; ++cfg) reject(reg::CONFIG, cfg);
        for (unsigned cmd : {1u, 2u, 3u, 0xffffffffu}) reject(reg::CTRL, cmd);
        check(peek(reg::STATUS) == 0 && peek(reg::CTRL) == 12, "busy rejects preserve job/config");
        wr(reg::CTRL, 0);
        unsigned char disabled = 0;
        std::uint32_t value = 0xffffffff;
        for (unsigned i = 0; i < 8; ++i)
            check(access(true, reg::key(i), value, &disabled, 1) == tlm::TLM_OK_RESPONSE, "disabled busy key write");
        for (unsigned i = 0; i < 4; ++i)
            check(access(true, reg::block(i), value, &disabled, 1) == tlm::TLM_OK_RESPONSE, "disabled busy block write");
        poll(command == reg::INIT ? reg::READY : reg::VALID);
    }
    output(cipher256); // Detect mutation from any rejected key/block/config write.
    next(); output(cipher256); // Disabled writes must not invalidate the key schedule.
}
void reset_boundaries() {
    for (bool wide : {false, true})
        for (unsigned command : {reg::INIT, reg::NEXT})
            for (bool after : {false, true}) {
                program(wide); input(plain);
                if (command == reg::NEXT) init();
                wr(reg::CTRL, command);
                const unsigned cycles = command == reg::INIT ? 16 : 64;
                const auto duration = period * cycles;
                wait(duration - sc_time(1, SC_PS));
                check(peek(reg::STATUS) == 0, "reset boundary before completion");
                if (after) {
                    wait(2, SC_PS);
                    check(peek(reg::STATUS) == (command == reg::INIT ? 1u : 3u), "completion before reset");
                }
                reset_request.notify(SC_ZERO_TIME); wait(reset_done);
                check(rd(reg::STATUS) == 1 && rd(reg::CTRL) == 0, "reset boundary clears job/config/status");
                for (unsigned i = 0; i < 4; ++i) check(rd(reg::result(i)) == 0, "reset boundary no stale word");
                wait(period * 70);
                check(rd(reg::STATUS) == 1, "cancelled completion cannot reassert VALID");
                reject(reg::CTRL, reg::NEXT);
            }
    vectors(false); vectors(true);
}
void debug_invariants() {
    program(true); input(plain); init(); next();
    const auto before = sc_time_stamp();
    for (unsigned offset = 0; offset < reg::APERTURE; offset += 4) {
        const auto value = peek(offset);
        unsigned char bytes[4]{0xff, 0xff, 0xff, 0xff};
        tlm::tlm_generic_payload tx;
        tx.set_command(tlm::TLM_WRITE_COMMAND); tx.set_address(offset);
        tx.set_data_ptr(bytes); tx.set_data_length(4); tx.set_streaming_width(4);
        check(cpu->transport_dbg(tx) == 0 && tx.get_response_status() == tlm::TLM_COMMAND_ERROR_RESPONSE,
              "all aligned debug writes rejected");
        check(peek(offset) == value, "debug write cannot alter visible registers");
    }
    check(sc_time_stamp() == before && peek(reg::STATUS) == 3, "debug sweep consumes no time or flags");
    next(); output(cipher256); // Also proves hidden WO storage/schedule was unchanged.
    wr(reg::CTRL, reg::NEXT);
    const auto pulse = peek(reg::CTRL);
    const auto busy = peek(reg::STATUS);
    for (unsigned repeat = 0; repeat < 100; ++repeat)
        check(peek(reg::CTRL) == pulse && peek(reg::STATUS) == busy, "debug polling cannot advance a job/pulse");
    poll(reg::VALID); output(cipher256);
}
void hds_fields() {
    // HDS literal CSR indices translated using the explicitly unapproved x4
    // VP addressing policy. This remains POLICY evidence, not SPEC sign-off.
    check(rd(0x00) == 0x61657320 && rd(0x04) == 0x20202020 && rd(0x08) == 0x302e3630,
          "HDS identity literals with VP address translation");
    for (unsigned mode = 0; mode < 4; ++mode) {
        const bool wide = mode & 2, encrypt = mode & 1;
        const auto& source = encrypt ? plain : (wide ? cipher256 : cipher128);
        const auto& expected = encrypt ? (wide ? cipher256 : cipher128) : plain;
        wr(0x28, mode | 0xfffffffcu);
        check(rd(0x28) == 0 && rd(0x20) == 4 * mode, "HDS CONFIG mask/WO and CTRL mirror");
        for (unsigned i = 0; i < 8; ++i) {
            wr(0x40 + 4*i, key_words[i]);
            check(rd(0x40 + 4*i) == 0, "HDS KEY write-only readback");
        }
        for (unsigned i = 0; i < 4; ++i) {
            wr(0x80 + 4*i, source[i]);
            check(rd(0x80 + 4*i) == 0, "HDS BLOCK write-only readback");
        }
        wr(0x20, 1);
        check(peek(0x20) == (4 * mode + 1), "HDS INIT pulse and mirror");
        poll(1); check((rd(0x24) & ~3u) == 0, "HDS STATUS reserved bits");
        wr(0x20, 2);
        check(peek(0x20) == (4 * mode + 2), "HDS NEXT pulse and mirror");
        poll(2);
        for (unsigned i = 0; i < 4; ++i)
            check(rd(0xc0 + 4*i) == expected[i], "HDS direction/key-length/word order under full-result policy");
    }
    for (unsigned index = 3; index < 0x30; ++index) {
        const bool reserved = index <= 7 || (index >= 0x0b && index <= 0x0f) ||
            (index >= 0x18 && index <= 0x1f) || (index >= 0x24 && index <= 0x2f);
        if (reserved) { wr(index * 4, 0xffffffff); check(rd(index * 4) == 0, "HDS stated reserved range"); }
    }
}
void sequence_oracle() {
    const auto all = load_vectors(vector_path);
    unsigned comparisons = 0;
    // Different blocks with one expanded zero key, both key lengths and
    // directions. These are the independently generated walking-input cases.
    for (bool wide : {false, true}) {
        wr(reg::CONFIG, wide ? 3 : 1);
        for (unsigned i = 0; i < 8; ++i) wr(reg::key(i), 0);
        init();
        const unsigned begin = (wide ? 515 : 0) + 3 + (wide ? 256 : 128);
        for (unsigned bit = 0; bit < 128; ++bit) {
            const auto& v = all.at(begin + bit);
            check(v.wide == wide, "walking-input reference key width");
            for (auto key_byte : v.key) check(key_byte == 0, "walking-input reference zero key");
            input(words(v.input));
            for (bool encrypt : {true, false}) {
                wr(reg::CONFIG, (wide ? 2u : 0u) | (encrypt ? 1u : 0u));
                next(); output(words(encrypt ? v.encrypted : v.decrypted)); ++comparisons;
            }
        }
    }
    // Reinitialize directly in either direction while alternating key widths;
    // this catches an INIT implementation that only works in encrypt mode.
    for (unsigned n = 0; n < 128; ++n) {
        const auto& v = all.at((n * 499u) % all.size());
        const bool encrypt = (n % 3) == 0;
        wr(reg::CONFIG, (v.wide ? 2u : 0u) | (encrypt ? 1u : 0u));
        for (unsigned i = 0; i < 8; ++i) {
            std::uint32_t word = 0;
            for (unsigned j = 0; j < 4; ++j) word = (word << 8) | v.key[4*i+j];
            wr(reg::key(i), word);
        }
        input(words(v.input)); init(); next();
        output(words(encrypt ? v.encrypted : v.decrypted)); ++comparisons;
    }
    check(comparisons == 640, "independent sequence comparison count");
    std::cout << "Sequence independent transform comparisons=" << comparisons << '\n';
}
#ifdef FU3_AES_BUS_TEST
void bus_lanes() {
    const auto burst = [&](bool write, std::uint64_t address, unsigned char* data, unsigned length, unsigned beat) {
        tlm::tlm_generic_payload tx;
        tx.set_command(write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
        tx.set_address(address); tx.set_data_ptr(data); tx.set_data_length(length); tx.set_streaming_width(length);
        fu3_bus::ScopedAccess metadata(tx, beat, fu3_bus::Burst::Increment);
        sc_time delay = SC_ZERO_TIME; transfer(tx, delay); if (delay != SC_ZERO_TIME) wait(delay);
        check(tx.get_address() == address && tx.get_data_length() == length && tx.get_data_ptr() == data,
              "bus restores original payload");
        return tx.get_response_status();
    };
    program(true);
    // Byte and halfword CPU accesses are expanded to strobed aligned words by the fabric.
    for (unsigned i = 0; i < 8; ++i) {
        wr(reg::key(i), 0);
        unsigned char data[4];
        for (unsigned j = 0; j < 4; ++j) data[j] = static_cast<unsigned char>(key_words[i] >> (8*j));
        const unsigned beat = i % 2 ? 2 : 1;
        check(burst(true, base + reg::key(i), data, 4, beat) == tlm::TLM_OK_RESPONSE, "narrow key burst");
    }
    input(plain); init(); next();
    unsigned char data[16]{};
    check(burst(false, base + reg::RESULT0, data, 16, 4) == tlm::TLM_OK_RESPONSE, "four-word result burst");
    for (unsigned i = 0; i < 16; ++i)
        check(data[i] == static_cast<unsigned char>(cipher256[i / 4] >> (8*(i % 4))), "result burst byte order");
    check(burst(false, base + 0x400, data, 4, 4) == tlm::TLM_ADDRESS_ERROR_RESPONSE, "model aperture boundary");
    check(burst(false, base + 0x10000, data, 4, 4) == tlm::TLM_ADDRESS_ERROR_RESPONSE, "fabric aperture boundary");
    check(burst(true, base + reg::KEY0 + 1, data, 2, 2) == tlm::TLM_ADDRESS_ERROR_RESPONSE, "unaligned halfword");
    check(burst(false, base + 0xfffc, data, 8, 4) == tlm::TLM_ADDRESS_ERROR_RESPONSE, "cross-window burst");
    output(cipher256);
}
#endif
