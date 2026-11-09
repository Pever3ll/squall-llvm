# Running a compiled image on hardware

What a loader, a board, or a testbench must do to run an object produced by
`clang --target=squallv2` or `llc -mtriple=squallv2`. Until now this was
written down only in Isacomp's testbenches (`isa/squall/squall_prod_tb.v`,
`test/squall_v2_fixtures.py`) and in `squall_tools/rtl.py`, which follow it.

Every number below comes from the image's manifest, the JSON in the object's
`.squall.config` section (`squall-image` writes it as `manifest.json`). Field
names are in `code`.

## 1. Check that the image is for this part

| Check | Why |
| --- | --- |
| `format` is `squall-v2-image-2` and `abi` is `squall-frame-slots-2` | Older images use another encoding and tag layout |
| `isa.commit` is the Isacomp commit the hardware was generated from | The encoding, the tag layout (ip 28:0, fp 60:32, port 61) and zero-extended slot reads are those of that commit |
| `memory_bytes` equals the part's memory size | The frame arena, the token stack and the reset values all depend on it. The core decodes a fixed address width (`mem memory[18:0]` in `fetch.isa` is 512 KiB), and neither it nor the image can detect a mismatch at run time |
| The object has no relocations against `.squall.image` | Nothing links Squall objects; an unresolved field is zero |

`squall-image` makes all four checks (the third with `--memory-bytes N`).
The compiler's default is 524288 bytes. `-squall-memory-bytes` accepts more,
and the compiler suggests a larger value when a program does not fit, but an
image larger than the part cannot run on it: Embench `cubic`, for one, needs
983040 bytes.

## 2. Load all of memory

The image is the complete contents of memory, `memory_bytes` long, as
little-endian 64-bit words. **Every word must be written before reset is
released, and most of the image is not zero.**

* The frame arena, words `root_frame_word + 1` up to but not including
  `frame_arena_end_word`, holds `frame_arena_fill` (`0x8000000000000000`, an
  empty constant slot) in every word: about 47,000 words, 368 KiB, of a default
  image. A loader may write that range as a fill rather than transfer it.
* A register store is a Const-rule match. On a word whose presence bits
  (63:62) are `00` it parks its token and forms none (`slot.isa`: `parks`), so
  on a zero-filled arena the first store of the entry function ends the
  program: the stack drains, and no status is ever written.
* Entry arguments are written by the loader into `argument_words` as constant
  slots, `0x8000000000000000 | value`, with the value converted as
  `argument_types` and `argument_extensions` say and zero-extended from 32
  bits. `squall-image --arg` does this.

## 3. Reset

The core has two stack pointer registers whose reset values are inputs
(`reset_stack_base_rd`, `reset_stack_top_rd` on the generated cores). Drive
them with `reset.stack_base` and `reset.stack_top`, byte addresses. They are
`memory_bytes - 16` and `memory_bytes - 32`: one start token is on the stack,
at `stack_base`, and the image already contains it.

## 4. Completion

The program has ended when the token stack is empty and nothing is in flight:
`stack_top == stack_base`, and it stays there for a full pipeline length
(the testbench waits `pipeline_length` cycles; 6 on the production core). The
core exports no single "halted" signal that accounts for tokens in the
pipeline, so a host must apply the same rule.

A program that never ends is indistinguishable from a long one, so run with a
cycle budget.

## 5. Flush, then read the result

On a core with write-back data caches, the result and status are in the cache
when the program ends. Raise every `*_flush_all` input and wait for every
`*_flush_done` before reading memory (`wire_cache_flush` in the fixtures).
Reading earlier returns the image's initial zeros.

| Word | Contents |
| --- | --- |
| `result_word` (1) | The entry function's return value in bits 31:0 |
| `status_word` (2) | See below |

| Status | Meaning | Result valid |
| --- | --- | --- |
| 1 | The entry function returned, or `exit` was called | Yes |
| 2 | A call would have passed the end of the frame arena (recursion too deep) | No |
| 3 | A load or store through a pointer that is misaligned, below the globals, or at or above `frame_arena_end_word` | No |
| 4 | `__builtin_trap`, `abort`, a failed `assert`, or a call through a null function pointer | No |
| 0 | **The program drained without reporting.** The last token was discarded | No |

Status 0 with an empty stack means a token was lost. The ways that can happen:

* a jump or send to word 0 or into alignment padding (padding words are
  `write 0 0x00`, which discard a token);
* a store through a wild pointer that passed the range check and landed on a
  word with empty presence bits, such as an instruction word or the unused
  words between the code and the arena;
* memory that was not loaded as section 2 requires.

A core that is still running at the end of its budget is in a loop, in the
program or because a wild jump landed on a word that sends to itself: an
arena word executed as an instruction does.

## 6. Seeing more than one word

Compiled code can reach no device: every pointer is checked against the
image's data regions, and there is no I/O instruction it can use. To observe
progress on hardware, keep it in memory and read it back after the flush:

```c
volatile unsigned progress;        /* or a ring buffer */
int main(void) { progress = 1; step_one(); progress = 2; ... }
```

`globals` in the manifest maps each global's name to its word address; a
global's cell `i` is word `globals[name] + i`, value in bits 31:0. On a run
that ended with status 0, or was stopped at its budget, `progress` says how
far it got.

## What the compiler does not protect

* A store through a wild pointer into the code, the globals or another frame
  is not detected. Only the low reserved words and the token stack are
  guarded. On a cached core, a store into an instruction word may also not be
  seen by the instruction cache.
* The token stack's capacity (`stack_token_capacity`, 1024 tokens) is a
  reservation, not a check. Compiled code keeps at most a few tokens live
  (two, plus one per nested fork/join region), so it is not reached in
  practice.
* `-squall-fork-join` is off by default. It relies on token interleavings
  that only the pipelined cores exercise; leave it off for first bring-up.
