#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <bytestream.h>
#include <disasmstream.h>
#include <instruction.h>

#include "avr_instruction_set.h"
#include "avr_support.h"

/******************************************************************************/
/* AVR Instruction/Directive Accessor Functions */
/******************************************************************************/

extern uint32_t avr_instruction_get_address(struct instruction *instr);
extern unsigned int avr_instruction_get_width(struct instruction *instr);
extern unsigned int avr_instruction_get_num_operands(struct instruction *instr);
extern unsigned int avr_instruction_get_opcodes(struct instruction *instr, uint8_t *dest);
extern int avr_instruction_get_str_address_label(struct instruction *instr, char *dest, int size, int flags);
extern int avr_instruction_get_str_address(struct instruction *instr, char *dest, int size, int flags);
extern int avr_instruction_get_str_opcodes(struct instruction *instr, char *dest, int size, int flags);
extern int avr_instruction_get_str_mnemonic(struct instruction *instr, char *dest, int size, int flags);
extern int avr_instruction_get_str_operand(struct instruction *instr, char *dest, int size, int index, int flags);
extern int avr_instruction_get_str_comment(struct instruction *instr, char *dest, int size, int flags);
extern void avr_instruction_free(struct instruction *instr);

extern unsigned int avr_directive_get_num_operands(struct instruction *instr);
extern int avr_directive_get_str_mnemonic(struct instruction *instr, char *dest, int size, int flags);
extern int avr_directive_get_str_operand(struct instruction *instr, char *dest, int size, int index, int flags);
extern void avr_directive_free(struct instruction *instr);

/******************************************************************************/
/* AVR Disassembly Stream Support */
/******************************************************************************/

struct disasmstream_avr_state {
    /* 4-byte opcode buffer */
    uint8_t data[4];
    uint32_t address[4];
    unsigned int len;

    /* initialized, eof encountered booleans */
    int initialized, eof;
    /* Next expected address */
    uint32_t next_address;
};

int disasmstream_avr_init(struct DisasmStream *self) {
    /* Allocate stream state */
    self->state = malloc(sizeof(struct disasmstream_avr_state));
    if (self->state == NULL) {
        self->error = "Error allocating disasm stream state!";
        return STREAM_ERROR_ALLOC;
    }
    /* Initialize stream state */
    memset(self->state, 0, sizeof(struct disasmstream_avr_state));

    /* Reset the error to NULL */
    self->error = NULL;

    /* Initialize the input stream */
    if (self->in->stream_init(self->in) < 0) {
        self->error = "Error in input stream initialization!";
        return STREAM_ERROR_INPUT;
    }

    return 0;
}

int disasmstream_avr_close(struct DisasmStream *self) {
    /* Free stream state memory */
    free(self->state);

    /* Close input stream */
    if (self->in->stream_close(self->in) < 0) {
        self->error = "Error in input stream close!";
        return STREAM_ERROR_INPUT;
    }

    return 0;
}

/******************************************************************************/
/* Core of the AVR Disassembler */
/******************************************************************************/

static int util_disasm_directive(struct instruction *instr, char *name, uint32_t value);
static int util_disasm_instruction(struct instruction *instr, struct avrInstructionInfo *instructionInfo, struct disasmstream_avr_state *state);
static void util_disasm_operands(struct avrInstructionDisasm *instructionDisasm);
static int32_t util_disasm_operand(struct avrInstructionInfo *instructionInfo, uint32_t operand, int index);
static void util_opbuffer_shift(struct disasmstream_avr_state *state, int n);
static int util_opbuffer_len_consecutive(struct disasmstream_avr_state *state);
static struct avrInstructionInfo *util_iset_lookup_by_opcode(uint16_t opcode);
static int util_bits_data_from_mask(uint16_t data, uint16_t mask);

int disasmstream_avr_read(struct DisasmStream *self, struct instruction *instr) {
    struct disasmstream_avr_state *state = (struct disasmstream_avr_state *)self->state;
    int decodeAttempts, lenConsecutive;

    /* Clear the destination instruction structure */
    memset(instr, 0, sizeof(struct instruction));

    for (decodeAttempts = 0; decodeAttempts < sizeof(state->data)+1; decodeAttempts++) {
        /* Count the number of consective bytes in our opcode buffer */
        lenConsecutive = util_opbuffer_len_consecutive(state);

        /* If we decoded all bytes, reached EOF, then return EOF too */
        if (lenConsecutive == 0 && state->len == 0 && state->eof)
            return STREAM_EOF;

        /* If the address jumped since the last instruction or we're
         * uninitialized, then return an org directive */
        if (lenConsecutive > 0 && (state->address[0] != state->next_address || !state->initialized)) {
            /* Emit an origin directive */
            if (util_disasm_directive(instr, AVR_DIRECTIVE_NAME_ORIGIN, state->address[0]) < 0) {
                self->error = "Error allocating memory for directive!";
                return STREAM_ERROR_FAILURE;
            }
            /* Update our state's next expected address */
            state->next_address = state->address[0];
            state->initialized = 1;
            return 0;
        }

        /* Edge case: when input stream changes address or reaches EOF with 1
         * undecoded byte */
        if (lenConsecutive == 1 && (state->len > 1 || state->eof)) {
            /* Disassembly a raw .DB byte "instruction" */
            if (util_disasm_instruction(instr, &AVR_Instruction_Set[AVR_ISET_INDEX_BYTE], state) < 0) {
                self->error = "Error allocating memory for disassembled instruction!";
                return STREAM_ERROR_FAILURE;
            }
            return 0;
        }

        /* Two or more consecutive bytes */
        if (lenConsecutive >= 2) {
            struct avrInstructionInfo *instructionInfo;
            uint16_t opcode;

            /* Assemble the 16-bit opcode from little-endian input */
            opcode = (uint16_t)(state->data[1] << 8) | (uint16_t)(state->data[0]);
            /* Look up the instruction in our instruction set */
            if ( (instructionInfo = util_iset_lookup_by_opcode(opcode)) == NULL) {
                /* This should never happen because of the .DW instruction that
                 * matches any 16-bit opcode */
                self->error = "Error, catastrophic failure! Malformed instruction set!";
                return STREAM_ERROR_FAILURE;
            }

            /* If this is a 16-bit wide instruction */
            if (instructionInfo->width == 2) {
                /* Disassemble and return a 16-bit instruction */
                if (util_disasm_instruction(instr, instructionInfo, state) < 0) {
                    self->error = "Error allocating memory for disassembled instruction!";
                    return STREAM_ERROR_FAILURE;
                }
                return 0;

            /* Else, this is a 32-bit wide instruction */
            } else {
                /* We have read the complete 32-bit instruction */
                if (lenConsecutive == 4) {
                    /* Disassemble and return a 16-bit instruction */
                    if (util_disasm_instruction(instr, instructionInfo, state) < 0) {
                        self->error = "Error allocating memory for disassembled instruction!";
                        return STREAM_ERROR_FAILURE;
                    }
                    return 0;

                /* Edge case: when input stream changes address or reaches EOF
                 * with 3 or 2 undecoded long instruction bytes */
                } else if ((lenConsecutive == 3 && (state->len > 3 || state->eof)) ||
                           (lenConsecutive == 2 && (state->len > 2 || state->eof))) {
                    /* Return a raw .DW word "instruction" */
                    if (util_disasm_instruction(instr, &AVR_Instruction_Set[AVR_ISET_INDEX_WORD], state) < 0) {
                        self->error = "Error allocating memory for disassembled instruction!";
                        return STREAM_ERROR_FAILURE;
                    }
                    return 0;
                }

                /* Otherwise, read another byte into our opcode buffer below */
            }
        }

        uint8_t readData;
        uint32_t readAddr;
        int ret;

        /* Read the next data byte from the opcode stream */
        ret = self->in->stream_read(self->in, &readData, &readAddr);
        if (ret == STREAM_EOF) {
            /* Record encountered EOF */
            state->eof = 1;
        } else if (ret < 0) {
            self->error = "Error in opcode stream read!";
            return STREAM_ERROR_INPUT;
        }

        if (ret == 0) {
            /* If we have an opcode buffer overflow (this should never happen
             * if the decoding logic above is correct) */
            if (state->len == sizeof(state->data)) {
                self->error = "Error, catastrophic failure! Opcode buffer overflowed!";
                return STREAM_ERROR_FAILURE;
            }

            /* Append the data / address to our opcode buffer */
            state->data[state->len] = readData;
            state->address[state->len] = readAddr;
            state->len++;
        }
    }

    /* We should have returned an instruction above */
    self->error = "Error, catastrophic failure! No decoding logic invoked!";
    return STREAM_ERROR_FAILURE;
}

static int util_disasm_directive(struct instruction *instr, char *name, uint32_t value) {
    struct avrDirective *directive;

    /* Allocate directive structure */
    directive = malloc(sizeof(struct avrDirective));
    if (directive == NULL)
        return -1;

    /* Clear the structure */
    memset(directive, 0, sizeof(struct avrDirective));

    /* Load name and value */
    directive->name = name;
    directive->value = value;

    /* Setup the instruction structure */
    instr->data = directive;
    instr->type = DISASM_TYPE_DIRECTIVE;
    instr->get_num_operands = avr_directive_get_num_operands;
    instr->get_str_mnemonic = avr_directive_get_str_mnemonic;
    instr->get_str_operand = avr_directive_get_str_operand;
    instr->free = avr_directive_free;

    return 0;
}

static int util_disasm_instruction(struct instruction *instr, struct avrInstructionInfo *instructionInfo, struct disasmstream_avr_state *state) {
    struct avrInstructionDisasm *instructionDisasm;
    int i;

    /* Allocate disassembled instruction structure */
    instructionDisasm = malloc(sizeof(struct avrInstructionDisasm));
    if (instructionDisasm == NULL)
        return -1;

    /* Clear the structure */
    memset(instructionDisasm, 0, sizeof(struct avrInstructionDisasm));

    /* Load instruction info, address, opcodes, and operands */
    instructionDisasm->instructionInfo = instructionInfo;
    instructionDisasm->address = state->address[0];
    for (i = 0; i < instructionInfo->width; i++)
        instructionDisasm->opcode[i] = state->data[i];
    util_disasm_operands(instructionDisasm);
    util_opbuffer_shift(state, instructionInfo->width);

    /* Setup the instruction structure */
    instr->data = instructionDisasm;
    instr->type = DISASM_TYPE_INSTRUCTION;
    instr->get_address = avr_instruction_get_address;
    instr->get_width = avr_instruction_get_width;
    instr->get_num_operands = avr_instruction_get_num_operands;
    instr->get_opcodes = avr_instruction_get_opcodes;
    instr->get_str_address_label = avr_instruction_get_str_address_label;
    instr->get_str_address = avr_instruction_get_str_address;
    instr->get_str_opcodes = avr_instruction_get_str_opcodes;
    instr->get_str_mnemonic = avr_instruction_get_str_mnemonic;
    instr->get_str_operand = avr_instruction_get_str_operand;
    instr->get_str_comment = avr_instruction_get_str_comment;
    instr->free = avr_instruction_free;

    /* Update our state's next expected address */
    state->next_address = instructionDisasm->address + instructionDisasm->instructionInfo->width;

    return 0;
}

static void util_disasm_operands(struct avrInstructionDisasm *instructionDisasm) {
    struct avrInstructionInfo *instructionInfo = instructionDisasm->instructionInfo;
    int i;
    uint16_t opcode;
    uint32_t operand;

    opcode = ((uint16_t)instructionDisasm->opcode[1] << 8) | ((uint16_t)instructionDisasm->opcode[0]);

    /* Disassemble the operands */
    for (i = 0; i < instructionInfo->numOperands; i++) {
        /* Extract the operand bits */
        operand = util_bits_data_from_mask(opcode, instructionInfo->operandMasks[i]);

        /* Append the extra bits if it's a long operand */
        if (instructionInfo->operandTypes[i] == OPERAND_LONG_ABSOLUTE_ADDRESS)
            operand = (uint32_t)(operand << 16) | (uint32_t)(instructionDisasm->opcode[3] << 8) | (uint32_t)(instructionDisasm->opcode[2]);

        /* Disassemble the operand */
        instructionDisasm->operandDisasms[i] = util_disasm_operand(instructionInfo, operand, i);
    }
}

static int32_t util_disasm_operand(struct avrInstructionInfo *instructionInfo, uint32_t operand, int index) {
    int32_t operandDisasm;

    switch (instructionInfo->operandTypes[index]) {
        case OPERAND_BRANCH_ADDRESS:
            /* Relative branch address is 7 bits, two's complement form */

            /* If the sign bit is set */
            if (operand & (1 << 6)) {
                /* Manually sign-extend to the 32-bit container */
                operandDisasm = (int32_t) ( ( ~operand + 1 ) & 0x7f );
                operandDisasm = -operandDisasm;
            } else {
                operandDisasm = (int32_t) ( operand & 0x7f );
            }
            /* Multiply by two to point to a byte address */
            operandDisasm *= 2;

            break;
        case OPERAND_RELATIVE_ADDRESS:
             /* Relative address is 12 bits, two's complement form */

            /* If the sign bit is set */
            if (operand & (1 << 11)) {
                /* Manually sign-extend to the 32-bit container */
                operandDisasm = (int32_t) ( ( ~operand + 1 ) & 0xfff );
                operandDisasm = -operandDisasm;
            } else {
                operandDisasm = (int32_t) ( operand & 0xfff );
            }
            /* Multiply by two to point to a byte address */
            operandDisasm *= 2;

            break;
        case OPERAND_LONG_ABSOLUTE_ADDRESS:
            /* Multiply by two to point to a byte address */
            operandDisasm = operand * 2;
            break;
        case OPERAND_REGISTER_STARTR16:
            /* Register offset from R16 */
            operandDisasm = 16 + operand;
            break;
        case OPERAND_REGISTER_EVEN_PAIR:
            /* Register even */
            operandDisasm = operand*2;
            break;
        case OPERAND_REGISTER_EVEN_PAIR_STARTR24:
            /* Register even offset from R24 */
            operandDisasm = 24 + operand*2;
            break;
        default:
            /* Copy the operand with no additional processing */
            operandDisasm = operand;
            break;
    }

    return operandDisasm;
}

static void util_opbuffer_shift(struct disasmstream_avr_state *state, int n) {
    int i, j;

    for (i = 0; i < n; i++) {
        /* Shift the data and address slots down by one */
        for (j = 0; j < sizeof(state->data) - 1; j++) {
            state->data[j] = state->data[j+1];
            state->address[j] = state->address[j+1];
        }
        state->data[j] = 0x00;
        state->address[j] = 0x00;
        /* Update the opcode buffer length */
        if (state->len > 0)
            state->len--;
    }
}

static int util_opbuffer_len_consecutive(struct disasmstream_avr_state *state) {
    int i, lenConsecutive;

    lenConsecutive = 0;
    for (i = 0; i < state->len; i++) {
        /* If there is a greater than 1 byte gap between addresses */
        if (i > 0 && (state->address[i] - state->address[i-1]) != 1)
            break;
        lenConsecutive++;
    }

    return lenConsecutive;
}

static struct avrInstructionInfo *util_iset_lookup_by_opcode(uint16_t opcode) {
    int i, j;

    uint16_t instructionBits;

    for (i = 0; i < AVR_TOTAL_INSTRUCTIONS; i++) {
        instructionBits = opcode;

        /* Mask out the operands from the opcode */
        for (j = 0; j < AVR_Instruction_Set[i].numOperands; j++)
            instructionBits &= ~(AVR_Instruction_Set[i].operandMasks[j]);

        /* Compare left over instruction bits with the instruction mask */
        if (instructionBits == AVR_Instruction_Set[i].instructionMask)
            return &AVR_Instruction_Set[i];
    }

    return NULL;
}

static int util_bits_data_from_mask(uint16_t data, uint16_t mask) {
    uint16_t result;
    int i, j;

    result = 0;

    /* Sweep through mask from bits 0 to 15 */
    for (i = 0, j = 0; i < 16; i++) {
        /* If mask bit is set */
        if (mask & (1 << i)) {
            /* If data bit is set */
            if (data & (1 << i))
                result |= (1 << j);
            j++;
        }
    }

    return result;
}

