/***************************************************************************************************

  Zyan Disassembler Library (Zydis)

  Original Author : Florian Bernd

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.

***************************************************************************************************/

#include <string.h>
#include <Zydis/Status.h>
#include <Zydis/Decoder.h>
#include <InstructionTable.h>

/* ============================================================================================== */
/* Internal enums and types                                                                       */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Decoder context                                                                                */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the @c ZydisDecoderContext struct.
 */
typedef struct ZydisDecoderContext_
{
    /**
     * @brief   A pointer to the @c ZydisInstructionDecoder instance.
     */
    const ZydisInstructionDecoder* decoder;
    /**
     * @brief   The input buffer.
     */
    const uint8_t* buffer;
    /**
     * @brief   The input buffer length.
     */
    size_t bufferLen;
    /**
     * @brief   Contains the last (significant) segment prefix.
     */
    uint8_t lastSegmentPrefix;
    /**
     * @brief   Contains the prefix that should be traited as the mandatory-prefix, if the current
     *          instruction needs one.
     *          
     *          The last 0xF3/0xF2 prefix has precedence over previous ones and 0xF3/0xF2 in 
     *          general has precedence over 0x66.
     */
    uint8_t mandatoryCandidate;

    uint8_t prefixes[ZYDIS_MAX_INSTRUCTION_LENGTH];

    /**
     * @brief   Contains the effective operand-size index.
     * 
     * 0 = 16 bit, 1 = 32 bit, 2 = 64 bit
     */
    uint8_t eoszIndex;
    /**
     * @brief   Contains the effective address-size index.
     * 
     * 0 = 16 bit, 1 = 32 bit, 2 = 64 bit
     */
    uint8_t easzIndex;
    /**
     * @brief   Contains some cached REX/XOP/VEX/EVEX/MVEX values to provide uniform access.
     */
    struct
    {
        uint8_t W;
        uint8_t R;
        uint8_t X;
        uint8_t B;
        uint8_t L;
        uint8_t LL;
        uint8_t R2;
        uint8_t V2;
        uint8_t v_vvvv;
        uint8_t mask;
    } cache;
    /**
     * @brief   Internal EVEX-specific information.
     */
    struct
    {
        /**
         * @brief   The EVEX tuple-type.
         */
        ZydisEVEXTupleType tupleType;
        /**
         * @brief   The EVEX element-size.
         */
        uint8_t elementSize;
    } evex;
    /**
     * @brief   Internal MVEX-specific information.
     */
    struct
    {
        /**
         * @brief   The MVEX functionality.
         */
        ZydisMVEXFunctionality functionality;
    } mvex;
} ZydisDecoderContext;

/* ---------------------------------------------------------------------------------------------- */
/* Register encodings                                                                             */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the @c ZydisRegisterEncoding struct.
 */
typedef uint8_t ZydisRegisterEncoding;

/**
 * @brief   Values that represent register-encodings.
 * 
 * These values are used in the @c ZydisCalcRegisterId function.
 */
enum ZydisRegisterEncodings
{
    ZYDIS_REG_ENCODING_INVALID,
    /**
     * @brief   The register-id is encoded as part of the opcode.
     * 
     * Possible extension by `REX/XOP/VEX/EVEX/MVEX.B`.
     */
    ZYDIS_REG_ENCODING_OPCODE,
    /**
     * @brief   The register-id is encoded in `modrm.reg`.
     * 
     * Possible extension by `EVEX/MVEX.R'` (vector only) and `REX/XOP/VEX/EVEX/MVEX.R`.
     */
    ZYDIS_REG_ENCODING_REG,
    /**
     * @brief   The register-id is encoded in `XOP/VEX/EVEX/MVEX.vvvv`.
     * 
     * Possible extension by `EVEX/MVEX.v'` (vector only).
     */
    ZYDIS_REG_ENCODING_NDSNDD,
    /**
     * @brief   The register-id is encoded in `modrm.rm`.
     * 
     * Possible extension by `EVEX/MVEX.X` (vector only) and `REX/XOP/VEX/EVEX/MVEX.B`.
     */
    ZYDIS_REG_ENCODING_RM,
    /**
     * @brief   The register-id is encoded in `modrm.rm` or `sib.base` (if SIB is present).
     * 
     * Possible extension by `REX/XOP/VEX/EVEX/MVEX.B`.
     */
    ZYDIS_REG_ENCODING_BASE,
    /**
     * @brief   The register-id is encoded in `sib.index`.
     * 
     * Possible extension by `REX/XOP/VEX/EVEX/MVEX.X`.
     */
    ZYDIS_REG_ENCODING_INDEX,
    /**
     * @brief   The register-id is encoded in `sib.index`.
     * 
     * Possible extension by `EVEX/MVEX.V'` (vector only) and `REX/XOP/VEX/EVEX/MVEX.X`.
     */
    ZYDIS_REG_ENCODING_VIDX,
    /**
     * @brief   The register-id is encoded in an additional 8-bit immediate value.
     * 
     * Bits [7:4] in 64-bit mode with possible extension by bit [3] (vector only), bits [7:5] for 
     * all other modes.
     */
    ZYDIS_REG_ENCODING_IS4,
    /**
     * @brief   The register-id is encoded in `EVEX.aaa/MVEX.kkk`.
     */
    ZYDIS_REG_ENCODING_MASK
};

/* ============================================================================================== */
/* Internal functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Input helper functions                                                                         */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Reads one byte from the current read-position of the input data-source.
 *
 * @param   context     A pointer to the @c ZydisDecoderContext instance.
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct.
 * @param   value       A pointer to the memory that receives the byte from the input data-source.
 *
 * @return  A zydis status code.
 *          
 * If not empty, the internal buffer of the @c ZydisDecoderContext instance is used as temporary
 * data-source, instead of reading the byte from the actual input data-source.
 * 
 * This function may fail, if the @c ZYDIS_MAX_INSTRUCTION_LENGTH limit got exceeded, or no more   
 * data is available.
 */
static ZydisStatus ZydisInputPeek(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint8_t* value)
{ 
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction); 
    ZYDIS_ASSERT(value);

    if (instruction->length >= ZYDIS_MAX_INSTRUCTION_LENGTH) 
    { 
        return ZYDIS_STATUS_INSTRUCTION_TOO_LONG; 
    } 

    if (context->bufferLen > 0)
    {
        *value = context->buffer[0];
        return ZYDIS_STATUS_SUCCESS;
    }

    return ZYDIS_STATUS_NO_MORE_DATA;    
}

/**
 * @brief   Increases the read-position of the input data-source by one byte.
 *
 * @param   context     A pointer to the @c ZydisDecoderContext instance
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct.
 *                  
 * This function is supposed to get called ONLY after a successfull call of @c ZydisInputPeek.
 * 
 * If not empty, the read-position of the @c ZydisDecoderContext instances internal buffer is
 * increased, instead of the actual input data-sources read-position.
 * 
 * This function increases the @c length field of the @c ZydisInstructionInfo struct by one and
 * adds the current byte to the @c data array.
 */
static void ZydisInputSkip(ZydisDecoderContext* context, ZydisDecodedInstruction* instruction)
{ 
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(instruction->length < ZYDIS_MAX_INSTRUCTION_LENGTH);

    instruction->data[instruction->length++] = context->buffer++[0];
    --context->bufferLen;
}

/**
 * @brief   Reads one byte from the current read-position of the input data-source and increases the
 *          read-position by one byte afterwards.
 *
 * @param   context     A pointer to the @c ZydisDecoderContext instance.
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct.
 * @param   value       A pointer to the memory that receives the byte from the input data-source.
 *
 * @return  A zydis status code.
 *          
 * This function acts like a subsequent call of @c ZydisInputPeek and @c ZydisInputSkip.
 */
static ZydisStatus ZydisInputNext(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint8_t* value)
{ 
    ZYDIS_ASSERT(context); 
    ZYDIS_ASSERT(instruction); 
    ZYDIS_ASSERT(value);

    if (instruction->length >= ZYDIS_MAX_INSTRUCTION_LENGTH) 
    { 
        return ZYDIS_STATUS_INSTRUCTION_TOO_LONG; 
    } 

    if (context->bufferLen > 0)
    {
        *value = context->buffer++[0];
        instruction->data[instruction->length++] = *value;
        --context->bufferLen;
        return ZYDIS_STATUS_SUCCESS;
    }

    return ZYDIS_STATUS_NO_MORE_DATA;
}

/**
 * @brief   Reads a variable amount of bytes from the current read-position of the input data-source 
 *          and increases the read-position by specified amount of bytes afterwards.
 *
 * @param   context         A pointer to the @c ZydisDecoderContext instance.
 * @param   instruction     A pointer to the @c ZydisDecodedInstruction struct.
 * @param   value           A pointer to the memory that receives the byte from the input 
 *                          data-source.
 * @param   numberOfBytes   The number of bytes to read from the input data-source.
 *
 * @return  A zydis status code.
 *          
 * This function acts like a subsequent call of @c ZydisInputPeek and @c ZydisInputSkip.
 */
static ZydisStatus ZydisInputNextBytes(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint8_t* value, uint8_t numberOfBytes)
{ 
    ZYDIS_ASSERT(context); 
    ZYDIS_ASSERT(instruction); 
    ZYDIS_ASSERT(value);

    if (instruction->length >= ZYDIS_MAX_INSTRUCTION_LENGTH) 
    { 
        return ZYDIS_STATUS_INSTRUCTION_TOO_LONG; 
    } 

    if (context->bufferLen >= numberOfBytes)
    {
        memcpy(&instruction->data[instruction->length], context->buffer, numberOfBytes);
        instruction->length += numberOfBytes;

        memcpy(value, context->buffer, numberOfBytes);
        context->buffer += numberOfBytes;
        context->bufferLen -= numberOfBytes;

        return ZYDIS_STATUS_SUCCESS;
    }

    return ZYDIS_STATUS_NO_MORE_DATA;
}

/* ---------------------------------------------------------------------------------------------- */
/* Decode functions                                                                               */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Decodes the REX-prefix.
 *
 * @param   context     A pointer to the @c ZydisDecoderContext struct.
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct.
 * @param   data        The REX byte.
 */
static void ZydisDecodeREX(ZydisDecoderContext* context, ZydisDecodedInstruction* instruction, 
    uint8_t data)
{
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT((data & 0xF0) == 0x40);

    instruction->attributes |= ZYDIS_ATTRIB_HAS_REX;
    instruction->details.rex.isDecoded = ZYDIS_TRUE;
    instruction->details.rex.data[0]   = data;
    instruction->details.rex.W         = (data >> 3) & 0x01;
    instruction->details.rex.R         = (data >> 2) & 0x01;
    instruction->details.rex.X         = (data >> 1) & 0x01;
    instruction->details.rex.B         = (data >> 0) & 0x01;

    // Update internal fields
    context->cache.W                   = instruction->details.rex.W;
    context->cache.R                   = instruction->details.rex.R;
    context->cache.X                   = instruction->details.rex.X;
    context->cache.B                   = instruction->details.rex.B;
}

/**
 * @brief   Decodes the XOP-prefix.
 *
 * @param   context     A pointer to the @c ZydisDecoderContext struct.
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct.
 * @param   data        The XOP bytes.
 *
 * @return  A zydis status code.
 */
static ZydisStatus ZydisDecodeXOP(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint8_t data[3])
{
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(data[0] == 0x8F);
    ZYDIS_ASSERT(((data[1] >> 0) & 0x1F) >= 8);

    instruction->attributes |= ZYDIS_ATTRIB_HAS_XOP;
    instruction->details.xop.isDecoded     = ZYDIS_TRUE;
    instruction->details.xop.data[0]       = 0x8F;
    instruction->details.xop.data[1]       = data[1];
    instruction->details.xop.data[2]       = data[2];
    instruction->details.xop.R             = (data[1] >> 7) & 0x01;
    instruction->details.xop.X             = (data[1] >> 6) & 0x01;
    instruction->details.xop.B             = (data[1] >> 5) & 0x01;
    instruction->details.xop.m_mmmm        = (data[1] >> 0) & 0x1F;

    if ((instruction->details.xop.m_mmmm < 0x08) || (instruction->details.xop.m_mmmm > 0x0A))
    {
        // Invalid according to the AMD documentation
        return ZYDIS_STATUS_INVALID_MAP;
    }

    instruction->details.xop.W             = (data[2] >> 7) & 0x01;
    instruction->details.xop.vvvv          = (data[2] >> 3) & 0x0F;
    instruction->details.xop.L             = (data[2] >> 2) & 0x01;
    instruction->details.xop.pp            = (data[2] >> 0) & 0x03; 

    // Update internal fields
    context->cache.W                       = instruction->details.xop.W;
    context->cache.R                       = 0x01 & ~instruction->details.xop.R;
    context->cache.X                       = 0x01 & ~instruction->details.xop.X;
    context->cache.B                       = 0x01 & ~instruction->details.xop.B;
    context->cache.L                       = instruction->details.xop.L;
    context->cache.LL                      = instruction->details.xop.L;
    context->cache.v_vvvv                  = (0x0F & ~instruction->details.xop.vvvv);

    return ZYDIS_STATUS_SUCCESS;
}

/**
 * @brief   Decodes the VEX-prefix.
 *
 * @param   context     A pointer to the @c ZydisDecoderContext struct.
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct.
 * @param   data        The VEX bytes.
 *
 * @return  A zydis status code.
 */
static ZydisStatus ZydisDecodeVEX(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint8_t data[3])
{
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT((data[0] == 0xC4) || (data[0] == 0xC5));

    instruction->attributes |= ZYDIS_ATTRIB_HAS_VEX;
    instruction->details.vex.isDecoded     = ZYDIS_TRUE;
    instruction->details.vex.data[0]       = data[0];
    switch (data[0])
    {
    case 0xC4:
        instruction->details.vex.data[1]   = data[1];
        instruction->details.vex.data[2]   = data[2];
        instruction->details.vex.R         = (data[1] >> 7) & 0x01;
        instruction->details.vex.X         = (data[1] >> 6) & 0x01;
        instruction->details.vex.B         = (data[1] >> 5) & 0x01;
        instruction->details.vex.m_mmmm    = (data[1] >> 0) & 0x1F;
        instruction->details.vex.W         = (data[2] >> 7) & 0x01;
        instruction->details.vex.vvvv      = (data[2] >> 3) & 0x0F;
        instruction->details.vex.L         = (data[2] >> 2) & 0x01;
        instruction->details.vex.pp        = (data[2] >> 0) & 0x03;
        break;
    case 0xC5:
        instruction->details.vex.data[1]   = data[1];
        instruction->details.vex.data[2]   = 0;
        instruction->details.vex.R         = (data[1] >> 7) & 0x01;
        instruction->details.vex.X         = 1;
        instruction->details.vex.B         = 1;
        instruction->details.vex.m_mmmm    = 1;
        instruction->details.vex.W         = 0;
        instruction->details.vex.vvvv      = (data[1] >> 3) & 0x0F;
        instruction->details.vex.L         = (data[1] >> 2) & 0x01;
        instruction->details.vex.pp        = (data[1] >> 0) & 0x03;
        break;
    default:
        ZYDIS_UNREACHABLE;
    }  

    // TODO: m_mmmm = 0 is only valid for some KNC instructions
    if (instruction->details.vex.m_mmmm > 0x03)
    {
        // Invalid according to the intel documentation
        return ZYDIS_STATUS_INVALID_MAP;
    }

    // Update internal fields
    context->cache.W                       = instruction->details.vex.W;
    context->cache.R                       = 0x01 & ~instruction->details.vex.R;
    context->cache.X                       = 0x01 & ~instruction->details.vex.X;
    context->cache.B                       = 0x01 & ~instruction->details.vex.B;
    context->cache.L                       = instruction->details.vex.L;
    context->cache.LL                      = instruction->details.vex.L;
    context->cache.v_vvvv                  = (0x0F & ~instruction->details.vex.vvvv);

    return ZYDIS_STATUS_SUCCESS;
}

/**
 * @brief   Decodes the EVEX-prefix.
 *
 * @param   context     A pointer to the @c ZydisDecoderContext struct.
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct.
 * @param   data        The EVEX bytes.
 *
 * @return  A zydis status code.
 */
static ZydisStatus ZydisDecodeEVEX(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint8_t data[4])
{
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(data[0] == 0x62);

    instruction->attributes |= ZYDIS_ATTRIB_HAS_EVEX;
    instruction->details.evex.isDecoded    = ZYDIS_TRUE;
    instruction->details.evex.data[0]      = 0x62;
    instruction->details.evex.data[1]      = data[1];
    instruction->details.evex.data[2]      = data[2];
    instruction->details.evex.data[3]      = data[3];
    instruction->details.evex.R            = (data[1] >> 7) & 0x01;
    instruction->details.evex.X            = (data[1] >> 6) & 0x01;
    instruction->details.evex.B            = (data[1] >> 5) & 0x01;
    instruction->details.evex.R2           = (data[1] >> 4) & 0x01;

    if (((data[1] >> 2) & 0x03) != 0x00)
    {
        // Invalid according to the intel documentation
        return ZYDIS_STATUS_MALFORMED_EVEX;
    }

    instruction->details.evex.mm           = (data[1] >> 0) & 0x03;

    if (instruction->details.evex.mm == 0x00)
    {
        // Invalid according to the intel documentation
        return ZYDIS_STATUS_INVALID_MAP;
    }

    instruction->details.evex.W            = (data[2] >> 7) & 0x01;
    instruction->details.evex.vvvv         = (data[2] >> 3) & 0x0F;

    ZYDIS_ASSERT(((data[2] >> 2) & 0x01) == 0x01);

    instruction->details.evex.pp           = (data[2] >> 0) & 0x03;
    instruction->details.evex.z            = (data[3] >> 7) & 0x01;
    instruction->details.evex.L2           = (data[3] >> 6) & 0x01;
    instruction->details.evex.L            = (data[3] >> 5) & 0x01;
    instruction->details.evex.b            = (data[3] >> 4) & 0x01;
    instruction->details.evex.V2           = (data[3] >> 3) & 0x01;

    if (!instruction->details.evex.V2 && 
        (context->decoder->machineMode != ZYDIS_MACHINE_MODE_LONG_64))
    {
        return ZYDIS_STATUS_MALFORMED_EVEX;
    }

    instruction->details.evex.aaa          = (data[3] >> 0) & 0x07;
    
    // Update internal fields
    context->cache.W                       = instruction->details.evex.W;
    context->cache.R                       = 0x01 & ~instruction->details.evex.R;
    context->cache.X                       = 0x01 & ~instruction->details.evex.X;
    context->cache.B                       = 0x01 & ~instruction->details.evex.B;
    context->cache.LL                      = (data[3] >> 5) & 0x03;
    context->cache.R2                      = 0x01 & ~instruction->details.evex.R2;
    context->cache.V2                      = 0x01 & ~instruction->details.evex.V2;
    context->cache.v_vvvv                  = 
        ((0x01 & ~instruction->details.evex.V2) << 4) | (0x0F & ~instruction->details.evex.vvvv);
    context->cache.mask                    = instruction->details.evex.aaa;

    if (!instruction->details.evex.V2 && (context->decoder->machineMode != 64))
    {
        return ZYDIS_STATUS_MALFORMED_EVEX;
    }
    if (!instruction->details.evex.b && (context->cache.LL == 3))
    {
        // LL = 3 is only valid for instructions with embedded rounding control
        return ZYDIS_STATUS_MALFORMED_EVEX;
    }

    return ZYDIS_STATUS_SUCCESS;
}

/**
 * @brief   Decodes the MVEX-prefix.
 *
 * @param   context     A pointer to the @c ZydisDecoderContext struct.
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct.
 * @param   data        The MVEX bytes.
 *
 * @return  A zydis status code.
 */
static ZydisStatus ZydisDecodeMVEX(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint8_t data[4])
{
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(data[0] == 0x62);

    instruction->attributes |= ZYDIS_ATTRIB_HAS_EVEX;
    instruction->details.mvex.isDecoded    = ZYDIS_TRUE;
    instruction->details.mvex.data[0]      = 0x62;
    instruction->details.mvex.data[1]      = data[1];
    instruction->details.mvex.data[2]      = data[2];
    instruction->details.mvex.data[3]      = data[3];
    instruction->details.mvex.R            = (data[1] >> 7) & 0x01;
    instruction->details.mvex.X            = (data[1] >> 6) & 0x01;
    instruction->details.mvex.B            = (data[1] >> 5) & 0x01;
    instruction->details.mvex.R2           = (data[1] >> 4) & 0x01;
    instruction->details.mvex.mmmm         = (data[1] >> 0) & 0x0F;

    if (instruction->details.mvex.mmmm > 0x03)
    {
        // Invalid according to the intel documentation
        return ZYDIS_STATUS_INVALID_MAP;
    }

    instruction->details.mvex.W            = (data[2] >> 7) & 0x01;
    instruction->details.mvex.vvvv         = (data[2] >> 3) & 0x0F;

    ZYDIS_ASSERT(((data[2] >> 2) & 0x01) == 0x00);

    instruction->details.mvex.pp           = (data[2] >> 0) & 0x03;
    instruction->details.mvex.E            = (data[3] >> 7) & 0x01;
    instruction->details.mvex.SSS          = (data[3] >> 4) & 0x07;
    instruction->details.mvex.V2           = (data[3] >> 3) & 0x01;
    instruction->details.mvex.kkk          = (data[3] >> 0) & 0x07; 
    
    // Update internal fields
    context->cache.W                       = instruction->details.mvex.W;
    context->cache.R                       = 0x01 & ~instruction->details.mvex.R;
    context->cache.X                       = 0x01 & ~instruction->details.mvex.X;
    context->cache.B                       = 0x01 & ~instruction->details.mvex.B;
    context->cache.R2                      = 0x01 & ~instruction->details.mvex.R2;
    context->cache.V2                      = 0x01 & ~instruction->details.mvex.V2;
    context->cache.LL                      = 2;
    context->cache.v_vvvv                  = 
        ((0x01 & ~instruction->details.mvex.V2) << 4) | (0x0F & ~instruction->details.mvex.vvvv);
    context->cache.mask                    = instruction->details.mvex.kkk;

    return ZYDIS_STATUS_SUCCESS;
}

/**
 * @brief   Decodes the ModRM-byte.
 *
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct.
 * @param   data        The modrm byte.
 */
static void ZydisDecodeModRM(ZydisDecodedInstruction* instruction, uint8_t data)
{
    ZYDIS_ASSERT(instruction);

    instruction->attributes |= ZYDIS_ATTRIB_HAS_MODRM;
    instruction->details.modrm.isDecoded   = ZYDIS_TRUE;
    instruction->details.modrm.data[0]     = data;
    instruction->details.modrm.mod         = (data >> 6) & 0x03;
    instruction->details.modrm.reg         = (data >> 3) & 0x07;
    instruction->details.modrm.rm          = (data >> 0) & 0x07;
}

/**
 * @brief   Decodes the SIB-byte.
 *
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct
 * @param   data        The sib byte.
 */
static void ZydisDecodeSIB(ZydisDecodedInstruction* instruction, uint8_t data)
{
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(instruction->details.modrm.isDecoded);
    ZYDIS_ASSERT(instruction->details.modrm.rm == 4);

    instruction->attributes |= ZYDIS_ATTRIB_HAS_SIB;
    instruction->details.sib.isDecoded = ZYDIS_TRUE;
    instruction->details.sib.data[0]   = data;
    instruction->details.sib.scale     = (data >> 6) & 0x03;
    instruction->details.sib.index     = (data >> 3) & 0x07;
    instruction->details.sib.base      = (data >> 0) & 0x07;
}

/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Reads a displacement value.
 * 
 * @param   context     A pointer to the @c ZydisDecoderContext struct.
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct.
 * @param   size        The physical size of the displacement value.
 * 
 * @return  A zydis status code.
 */
static ZydisStatus ZydisReadDisplacement(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint8_t size)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(instruction->details.disp.size == 0);

    instruction->details.disp.size = size;
    instruction->details.disp.offset = instruction->length;

    switch (size)
    {
    case 8:
    {
        uint8_t value;
        ZYDIS_CHECK(ZydisInputNext(context, instruction, &value));
        instruction->details.disp.value = *(int8_t*)&value;
        break;
    }
    case 16:
    {
        uint16_t value;
        ZYDIS_CHECK(ZydisInputNextBytes(context, instruction, (uint8_t*)&value, 2));
        instruction->details.disp.value = *(int16_t*)&value;
        break;
    }
    case 32:
    {
        uint32_t value;
        ZYDIS_CHECK(ZydisInputNextBytes(context, instruction, (uint8_t*)&value, 4));
        instruction->details.disp.value = *(int32_t*)&value;
        break;
    }
    case 64:
    {
        uint64_t value;
        ZYDIS_CHECK(ZydisInputNextBytes(context, instruction, (uint8_t*)&value, 8));
        instruction->details.disp.value = *(int64_t*)&value;
        break;
    }
    default:
        ZYDIS_UNREACHABLE;
    }

    // TODO: Fix endianess on big-endian systems   

    return ZYDIS_STATUS_SUCCESS;
}

/**
 * @brief   Reads an immediate value.
 * 
 * @param   context     A pointer to the @c ZydisDecoderContext struct.
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct.
 * @param   id          The immediate id (either 0 or 1).
 * @param   size        The physical size of the immediate value.
 * @param   isSigned    Signals, if the immediate value is signed.
 * @param   isRelative  Signals, if the immediate value is a relative offset.
 * 
 * @return  A zydis status code.
 */
static ZydisStatus ZydisReadImmediate(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint8_t id, uint8_t size, ZydisBool isSigned, 
    ZydisBool isRelative)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT((id == 0) || (id == 1));
    ZYDIS_ASSERT(isSigned || !isRelative);
    ZYDIS_ASSERT(instruction->details.imm[id].size == 0);

    instruction->details.imm[id].size = size;
    instruction->details.imm[id].offset = instruction->length;
    instruction->details.imm[id].isSigned = isSigned;
    instruction->details.imm[id].isRelative = isRelative;
    switch (size)
    {
    case 8:
    {
        uint8_t value;
        ZYDIS_CHECK(ZydisInputNext(context, instruction, &value));
        if (isSigned)
        {
            instruction->details.imm[id].value.s = (int8_t)value;
        } else
        {
            instruction->details.imm[id].value.u = value;    
        }
        break;
    }
    case 16:
    {
        uint16_t value;
        ZYDIS_CHECK(ZydisInputNextBytes(context, instruction, (uint8_t*)&value, 2));
        if (isSigned)
        {
            instruction->details.imm[id].value.s = (int16_t)value;
        } else
        {
            instruction->details.imm[id].value.u = value;    
        }
        break;   
    }
    case 32:
    {
        uint32_t value;
        ZYDIS_CHECK(ZydisInputNextBytes(context, instruction, (uint8_t*)&value, 4));
        if (isSigned)
        {
            instruction->details.imm[id].value.s = (int32_t)value;
        } else
        {
            instruction->details.imm[id].value.u = value;    
        }
        break;
    }
    case 64:
    {
        uint64_t value;
        ZYDIS_CHECK(ZydisInputNextBytes(context, instruction, (uint8_t*)&value, 8));
        if (isSigned)
        {
            instruction->details.imm[id].value.s = (int64_t)value;
        } else
        {
            instruction->details.imm[id].value.u = value;    
        }
        break;
    }
    default:
        ZYDIS_UNREACHABLE;
    }

    // TODO: Fix endianess on big-endian systems

    return ZYDIS_STATUS_SUCCESS;    
}

/* ---------------------------------------------------------------------------------------------- */
/* Semantical instruction decoding                                                                */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Calculates the register-id for a specific register-encoding and register-class.
 *          
 * @param   context         A pointer to the @c ZydisDecoderContext struct.
 * @param   instruction     A pointer to the @c ZydisDecodedInstruction struct.
 * @param   encoding        The register-encoding.
 * @param   registerClass   The register-class.
 * 
 * @return  A zydis status code.
 * 
 * This function calculates the register-id by combining different fields and flags of previously
 * decoded structs.
 */
static uint8_t ZydisCalcRegisterId(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, ZydisRegisterEncoding encoding, 
    ZydisRegisterClass registerClass)
{
    switch (context->decoder->machineMode)
    {
    case 16:
    case 32:
        switch (encoding)
        {
        case ZYDIS_REG_ENCODING_OPCODE:
        {
            ZYDIS_ASSERT((registerClass == ZYDIS_REGCLASS_GPR8) ||
                         (registerClass == ZYDIS_REGCLASS_GPR16) ||
                         (registerClass == ZYDIS_REGCLASS_GPR32) ||
                         (registerClass == ZYDIS_REGCLASS_GPR64));
            uint8_t value = (instruction->opcode & 0x0F);
            if (value > 7)
            {
                value = value - 8;
            }
            return value;
        }
        case ZYDIS_REG_ENCODING_REG:
            ZYDIS_ASSERT(instruction->details.modrm.isDecoded);
            return instruction->details.modrm.reg;
        case ZYDIS_REG_ENCODING_NDSNDD:
            return context->cache.v_vvvv & 0x07;
        case ZYDIS_REG_ENCODING_RM:
            ZYDIS_ASSERT(instruction->details.modrm.isDecoded);
            return instruction->details.modrm.rm;
        case ZYDIS_REG_ENCODING_BASE:
            ZYDIS_ASSERT(instruction->details.modrm.isDecoded);
            ZYDIS_ASSERT(instruction->details.modrm.mod != 3);
            if (instruction->details.modrm.rm == 4)
            {
                ZYDIS_ASSERT(instruction->details.sib.isDecoded);
                return instruction->details.sib.base;
            }
            return instruction->details.modrm.rm;
        case ZYDIS_REG_ENCODING_INDEX:
            ZYDIS_ASSERT(instruction->details.modrm.isDecoded);
            ZYDIS_ASSERT(instruction->details.modrm.mod != 3);
            ZYDIS_ASSERT(instruction->details.sib.isDecoded);
            return instruction->details.sib.index;
        case ZYDIS_REG_ENCODING_VIDX:
            ZYDIS_ASSERT(instruction->details.modrm.isDecoded);
            ZYDIS_ASSERT(instruction->details.modrm.mod != 3);
            ZYDIS_ASSERT(instruction->details.sib.isDecoded);
            ZYDIS_ASSERT((registerClass == ZYDIS_REGCLASS_XMM) ||
                         (registerClass == ZYDIS_REGCLASS_YMM) ||
                         (registerClass == ZYDIS_REGCLASS_ZMM));
            return instruction->details.sib.index;
        case ZYDIS_REG_ENCODING_IS4:
            return (instruction->details.imm[0].value.u >> 5) & 0x07;
        case ZYDIS_REG_ENCODING_MASK:
            return context->cache.mask;
        default:
            ZYDIS_UNREACHABLE;
        }
    case 64:
        switch (encoding)
        {
        case ZYDIS_REG_ENCODING_OPCODE:
        {
            ZYDIS_ASSERT((registerClass == ZYDIS_REGCLASS_GPR8) ||
                         (registerClass == ZYDIS_REGCLASS_GPR16) ||
                         (registerClass == ZYDIS_REGCLASS_GPR32) ||
                         (registerClass == ZYDIS_REGCLASS_GPR64));
            uint8_t value = (instruction->opcode & 0x0F);
            if (value > 7)
            {
                value = value - 8;
            }
            return value | (context->cache.B << 3);
        }
        case ZYDIS_REG_ENCODING_REG:
        {
            ZYDIS_ASSERT(instruction->details.modrm.isDecoded);
            uint8_t value = instruction->details.modrm.reg;
            if (registerClass != ZYDIS_REGCLASS_MASK)
            {
                value |= (context->cache.R << 3);    
            }
            // R' only exists for EVEX and MVEX. No encoding check needed
            switch (registerClass)
            {
            case ZYDIS_REGCLASS_XMM:
            case ZYDIS_REGCLASS_YMM:
            case ZYDIS_REGCLASS_ZMM:              
                value |= (context->cache.R2 << 4);
                break;
            default:
                break;
            }
            return value;
        }
        case ZYDIS_REG_ENCODING_NDSNDD:
            // v' only exists for EVEX and MVEX. No encoding check needed
            switch (registerClass)
            {
            case ZYDIS_REGCLASS_XMM:
            case ZYDIS_REGCLASS_YMM:
            case ZYDIS_REGCLASS_ZMM:
                return context->cache.v_vvvv;
            case ZYDIS_REGCLASS_MASK:
                return context->cache.v_vvvv & 0x07;
            default:
                return context->cache.v_vvvv & 0x0F;
            }
        case ZYDIS_REG_ENCODING_RM:
        {
            ZYDIS_ASSERT(instruction->details.modrm.isDecoded);
            uint8_t value = instruction->details.modrm.rm;
            if (registerClass != ZYDIS_REGCLASS_MASK)
            {
                value |= (context->cache.B << 3);    
            }
            // We have to check the instruction-encoding, because the extension by X is only valid
            // for EVEX and MVEX instructions
            if ((instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX) ||
                (instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_MVEX))
            {
                switch (registerClass)
                {
                case ZYDIS_REGCLASS_XMM:
                case ZYDIS_REGCLASS_YMM:
                case ZYDIS_REGCLASS_ZMM:
                    value |= (context->cache.X << 4);
                    break;
                default:
                    break;
                }
            }
            return value;
        }
        case ZYDIS_REG_ENCODING_BASE:
            ZYDIS_ASSERT(instruction->details.modrm.isDecoded);
            ZYDIS_ASSERT(instruction->details.modrm.mod != 3);
            if (instruction->details.modrm.rm == 4)
            {
                ZYDIS_ASSERT(instruction->details.sib.isDecoded);
                return instruction->details.sib.base | (context->cache.B << 3);
            }
            return instruction->details.modrm.rm | (context->cache.B << 3);
        case ZYDIS_REG_ENCODING_INDEX:
            ZYDIS_ASSERT(instruction->details.modrm.isDecoded);
            ZYDIS_ASSERT(instruction->details.modrm.mod != 3);
            ZYDIS_ASSERT(instruction->details.sib.isDecoded);
            return instruction->details.sib.index | (context->cache.X << 3);
        case ZYDIS_REG_ENCODING_VIDX:
            ZYDIS_ASSERT(instruction->details.modrm.isDecoded);
            ZYDIS_ASSERT(instruction->details.modrm.mod != 3);
            ZYDIS_ASSERT(instruction->details.sib.isDecoded);
            ZYDIS_ASSERT((registerClass == ZYDIS_REGCLASS_XMM) ||
                         (registerClass == ZYDIS_REGCLASS_YMM) ||
                         (registerClass == ZYDIS_REGCLASS_ZMM));
            // v' only exists for EVEX and MVEX. No encoding check needed
            return instruction->details.sib.index | (context->cache.X << 3) | 
                (context->cache.V2 << 4);
        case ZYDIS_REG_ENCODING_IS4:
        {
            uint8_t value = (instruction->details.imm[0].value.u >> 4) & 0x0F;
            // We have to check the instruction-encoding, because the extension by bit [3] is only 
            // valid for EVEX and MVEX instructions
            if ((instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX) ||
                (instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_MVEX))
            {
                switch (registerClass)
                {
                case ZYDIS_REGCLASS_XMM:
                case ZYDIS_REGCLASS_YMM:
                case ZYDIS_REGCLASS_ZMM:
                    value |= ((instruction->details.imm[0].value.u & 0x08) << 1);
                default:
                    break;
                }
            }
            return value;
        }
        case ZYDIS_REG_ENCODING_MASK:
            return context->cache.mask;
        default:
            ZYDIS_UNREACHABLE;
        }
    default:
        ZYDIS_UNREACHABLE;
    }
    ZYDIS_UNREACHABLE;
    return 0;
}

/**
 * @brief   Sets the operand-size and element-specific information for the given operand.
 *
 * @param   context         A pointer to the @c ZydisDecoderContext instance.
 * @param   instruction     A pointer to the @c ZydisDecodedInstruction struct.
 * @param   operand         A pointer to the @c ZydisDecodedOperand struct.
 * @param   definition      A pointer to the @c ZydisOperandDefinition struct.
 */
static void ZydisSetOperandSizeAndElementInfo(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, ZydisDecodedOperand* operand, 
    const ZydisOperandDefinition* definition)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(operand);
    ZYDIS_ASSERT(definition);

    // Operand size
    switch (operand->type)
    {
    case ZYDIS_OPERAND_TYPE_REGISTER:
    {
        if (definition->size[context->eoszIndex])
        {
            operand->size = definition->size[context->eoszIndex] * 8;     
        } else
        {
            operand->size = (context->decoder->machineMode == 64) ? 
                ZydisRegisterGetWidth64(operand->reg) : ZydisRegisterGetWidth(operand->reg);
        }
        operand->elementType = ZYDIS_ELEMENT_TYPE_INT;
        operand->elementSize = operand->size;
        break;
    }
    case ZYDIS_OPERAND_TYPE_MEMORY:
        switch (instruction->encoding)
        {
        case ZYDIS_INSTRUCTION_ENCODING_DEFAULT:
        case ZYDIS_INSTRUCTION_ENCODING_3DNOW:
        case ZYDIS_INSTRUCTION_ENCODING_XOP:
        case ZYDIS_INSTRUCTION_ENCODING_VEX:
            if (operand->mem.isAddressGenOnly)
            {
                ZYDIS_ASSERT(definition->size[context->eoszIndex] == 0);
                operand->size = instruction->addressWidth; 
                operand->elementType = ZYDIS_ELEMENT_TYPE_INT;
            } else
            {
                ZYDIS_ASSERT(definition->size[context->eoszIndex]);
                operand->size = definition->size[context->eoszIndex] * 8;
            }
            break;
        case ZYDIS_INSTRUCTION_ENCODING_EVEX:
            if (definition->size[context->eoszIndex])
            {
                // Operand size is hardcoded
                operand->size = definition->size[context->eoszIndex] * 8;    
            } else
            {
                // Operand size depends on the tuple-type, the element-size and the number of 
                // elements
                ZYDIS_ASSERT(instruction->avx.vectorLength);
                ZYDIS_ASSERT(context->evex.elementSize);
                switch (context->evex.tupleType)
                {
                case ZYDIS_TUPLETYPE_FV:
                    if (instruction->avx.broadcast.mode)
                    {
                        operand->size = context->evex.elementSize;
                    } else
                    {
                        operand->size = instruction->avx.vectorLength;
                    }
                    break;
                case ZYDIS_TUPLETYPE_HV:
                    if (instruction->avx.broadcast.mode)
                    {
                        operand->size = context->evex.elementSize;
                    } else
                    {
                        operand->size = instruction->avx.vectorLength / 2;
                    }
                    break;
                default:
                    ZYDIS_UNREACHABLE;
                }
            }
            ZYDIS_ASSERT(operand->size);
            break;
        case ZYDIS_INSTRUCTION_ENCODING_MVEX:
            if (definition->size[context->eoszIndex])
            {
                // Operand size is hardcoded
                operand->size = definition->size[context->eoszIndex] * 8;    
            } else
            {
                ZYDIS_ASSERT(definition->elementType == ZYDIS_IELEMENT_TYPE_VARIABLE);
                ZYDIS_ASSERT(instruction->avx.vectorLength == 512);

                switch (instruction->avx.conversionMode)
                {
                case ZYDIS_CONVERSION_MODE_INVALID:
                    operand->size = 512;
                    switch (context->mvex.functionality)
                    {
                    case ZYDIS_MVEX_FUNC_SF_32:
                    case ZYDIS_MVEX_FUNC_SF_32_BCST_4TO16:
                    case ZYDIS_MVEX_FUNC_UF_32:
                    case ZYDIS_MVEX_FUNC_DF_32:
                        operand->elementType = ZYDIS_ELEMENT_TYPE_FLOAT32;
                        operand->elementSize = 32;
                        break;
                    case ZYDIS_MVEX_FUNC_SF_32_BCST:
                        operand->size = 256;
                        operand->elementType = ZYDIS_ELEMENT_TYPE_FLOAT32;
                        operand->elementSize = 32;
                        break;
                    case ZYDIS_MVEX_FUNC_SI_32:
                    case ZYDIS_MVEX_FUNC_SI_32_BCST_4TO16:
                    case ZYDIS_MVEX_FUNC_UI_32:
                    case ZYDIS_MVEX_FUNC_DI_32:
                        operand->elementType = ZYDIS_ELEMENT_TYPE_INT;
                        operand->elementSize = 32;
                        break;
                    case ZYDIS_MVEX_FUNC_SI_32_BCST:
                        operand->size = 256;
                        operand->elementType = ZYDIS_ELEMENT_TYPE_INT;
                        operand->elementSize = 32;
                        break;
                    case ZYDIS_MVEX_FUNC_SF_64:
                    case ZYDIS_MVEX_FUNC_UF_64:
                    case ZYDIS_MVEX_FUNC_DF_64:
                        operand->elementType = ZYDIS_ELEMENT_TYPE_FLOAT64;
                        operand->elementSize = 64;
                        break;
                    case ZYDIS_MVEX_FUNC_SI_64:
                    case ZYDIS_MVEX_FUNC_UI_64:
                    case ZYDIS_MVEX_FUNC_DI_64:
                        operand->elementType = ZYDIS_ELEMENT_TYPE_INT;
                        operand->elementSize = 64;
                        break;
                    default:
                        ZYDIS_UNREACHABLE;
                    }
                    break;
                case ZYDIS_CONVERSION_MODE_FLOAT16:
                    operand->size = 256;
                    operand->elementType = ZYDIS_ELEMENT_TYPE_FLOAT16;
                    operand->elementSize = 16;
                    break;
                case ZYDIS_CONVERSION_MODE_SINT16:
                    operand->size = 256;
                    operand->elementType = ZYDIS_ELEMENT_TYPE_INT;
                    operand->elementSize = 16;
                    break;
                case ZYDIS_CONVERSION_MODE_UINT16:
                    operand->size = 256;
                    operand->elementType = ZYDIS_ELEMENT_TYPE_UINT;
                    operand->elementSize = 16;
                    break;
                case ZYDIS_CONVERSION_MODE_SINT8:
                    operand->size = 128;
                    operand->elementType = ZYDIS_ELEMENT_TYPE_INT;
                    operand->elementSize = 8;
                    break;
                case ZYDIS_CONVERSION_MODE_UINT8:
                    operand->size = 128;
                    operand->elementType = ZYDIS_ELEMENT_TYPE_UINT;
                    operand->elementSize = 8;
                    break;
                default:
                    ZYDIS_UNREACHABLE;
                }
                
                switch (instruction->avx.broadcast.mode)
                {
                case ZYDIS_BROADCAST_MODE_INVALID:
                    // Nothing to do here         
                    break;
                case ZYDIS_BROADCAST_MODE_1_TO_8:
                case ZYDIS_BROADCAST_MODE_1_TO_16:
                    operand->size = operand->elementSize;
                    break;
                case ZYDIS_BROADCAST_MODE_4_TO_8:
                case ZYDIS_BROADCAST_MODE_4_TO_16:
                    operand->size = operand->elementSize * 4;
                    break;
                default:
                    ZYDIS_UNREACHABLE;
                }
            }
            break;
        default:
            ZYDIS_UNREACHABLE;
        }
        break;
    case ZYDIS_OPERAND_TYPE_POINTER:
        ZYDIS_ASSERT((instruction->details.imm[0].size == 16) || 
                     (instruction->details.imm[0].size == 32));
        ZYDIS_ASSERT( instruction->details.imm[1].size == 16);
        operand->size = instruction->details.imm[0].size + instruction->details.imm[1].size;
        break;
    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
        operand->size = definition->size[context->eoszIndex] * 8;
        break;
    default:
        ZYDIS_UNREACHABLE;
    }

    // Element-type and -size
    if (definition->elementType && (definition->elementType != ZYDIS_IELEMENT_TYPE_VARIABLE))
    {
        ZydisGetElementInfo(definition->elementType, &operand->elementType, &operand->elementSize);
        if (!operand->elementSize)
        {
            // The element size is the same as the operand size. This is used for single element
            // scaling operands 
            operand->elementSize = operand->size;
        }
    }

    // Element count
    operand->elementCount = 1;
    if (operand->elementSize && operand->size)
    {
        operand->elementCount = operand->size / operand->elementSize;
    }
}

/**
 * @brief   Decodes an register-operand.
 * 
 * @param   instruction     A pointer to the @c ZydisDecodedInstruction struct.
 * @param   operand         A pointer to the @c ZydisDecodedOperand struct.
 * @param   registerClass   The register class.
 * @param   registerId      The register id.
 *
 * @return  A zydis status code.
 */
static ZydisStatus ZydisDecodeOperandRegister(ZydisDecodedInstruction* instruction, 
    ZydisDecodedOperand* operand, ZydisRegisterClass registerClass, uint8_t registerId)
{
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(operand);

    operand->type = ZYDIS_OPERAND_TYPE_REGISTER;

    if (registerClass == ZYDIS_REGCLASS_GPR8)
    {
        if ((instruction->attributes & ZYDIS_ATTRIB_HAS_REX) && (registerId >= 4)) 
        {
            operand->reg = ZYDIS_REGISTER_SPL + (registerId - 4);
        } else 
        {
            operand->reg = ZYDIS_REGISTER_AL + registerId;
        }
        if (operand->reg > ZYDIS_REGISTER_R15B)
        {
            return ZYDIS_STATUS_BAD_REGISTER;
        }
    } else
    {
        operand->reg = ZydisRegisterEncode(registerClass, registerId);
        if (!operand->reg)
        {
            return ZYDIS_STATUS_BAD_REGISTER;
        }
        if ((operand->reg == ZYDIS_REGISTER_CR1) ||
           ((operand->reg >= ZYDIS_REGISTER_CR5) && (operand->reg <= ZYDIS_REGISTER_CR15) &&
            (operand->reg != ZYDIS_REGISTER_CR8)))
        {
            return ZYDIS_STATUS_BAD_REGISTER;
        }
        if ((operand->reg == ZYDIS_REGISTER_DR4) || (operand->reg == ZYDIS_REGISTER_DR5) ||
           ((operand->reg >= ZYDIS_REGISTER_DR8) && (operand->reg <= ZYDIS_REGISTER_DR15)))
        {
            return ZYDIS_STATUS_BAD_REGISTER;    
        }
    }

    return ZYDIS_STATUS_SUCCESS;
}

/**
 * @brief   Decodes a memory operand.
 *
 * @param   context             A pointer to the @c ZydisDecoderContext instance.
 * @param   instruction         A pointer to the @c ZydisDecodedInstruction struct.
 * @param   operand             A pointer to the @c ZydisDecodedOperand struct.
 * @param   vidxRegisterClass   The register-class to use as the index register-class for 
 *                              instructions with VSIB addressing.
 *
 * @return  A zydis status code.
 */
static ZydisStatus ZydisDecodeOperandMemory(ZydisDecoderContext* context,
    ZydisDecodedInstruction* instruction, ZydisDecodedOperand* operand, 
    ZydisRegisterClass vidxRegisterClass)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(operand);
    ZYDIS_ASSERT(instruction->details.modrm.isDecoded);
    ZYDIS_ASSERT(instruction->details.modrm.mod != 3);
    ZYDIS_ASSERT(!vidxRegisterClass || ((instruction->details.modrm.rm == 4) && 
        ((instruction->addressWidth == 32) || (instruction->addressWidth == 64))));

    operand->type = ZYDIS_OPERAND_TYPE_MEMORY;

    uint8_t modrm_rm = instruction->details.modrm.rm;
    uint8_t displacementSize = 0;
    switch (instruction->addressWidth)
    {
    case 16:
    {
        static const ZydisRegister bases[] = 
        { 
            ZYDIS_REGISTER_BX,   ZYDIS_REGISTER_BX,   ZYDIS_REGISTER_BP,   ZYDIS_REGISTER_BP, 
            ZYDIS_REGISTER_SI,   ZYDIS_REGISTER_DI,   ZYDIS_REGISTER_BP,   ZYDIS_REGISTER_BX 
        };
        static const ZydisRegister indices[] = 
        { 
            ZYDIS_REGISTER_SI,   ZYDIS_REGISTER_DI,   ZYDIS_REGISTER_SI,   ZYDIS_REGISTER_DI,
            ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE 
        };
        operand->mem.base = bases[modrm_rm];
        operand->mem.index = indices[modrm_rm];
        operand->mem.scale = 0;
        switch (instruction->details.modrm.mod)
        {
        case 0:
            if (modrm_rm == 6) 
            {
                displacementSize = 16;
                operand->mem.base = ZYDIS_REGISTER_NONE;
            }
            break;
        case 1:
            displacementSize = 8;
            break;
        case 2:
            displacementSize = 16;
            break;
        default:
            ZYDIS_UNREACHABLE;
        }
        break;
    }
    case 32:
    {
        operand->mem.base = ZYDIS_REGISTER_EAX + ZydisCalcRegisterId(context, instruction, 
            ZYDIS_REG_ENCODING_BASE, ZYDIS_REGCLASS_GPR32);
        switch (instruction->details.modrm.mod)
        {
        case 0:
            if (modrm_rm == 5)
            {
                if (context->decoder->machineMode == ZYDIS_MACHINE_MODE_LONG_64)
                {
                    operand->mem.base = ZYDIS_REGISTER_EIP;
                } else
                {
                    operand->mem.base = ZYDIS_REGISTER_NONE;
                }
                displacementSize = 32;
            }
            break;
        case 1:
            displacementSize = 8;
            break;
        case 2:
            displacementSize = 32;
            break;
        default:
            ZYDIS_UNREACHABLE;
        }
        if (modrm_rm == 4)
        {
            ZYDIS_ASSERT(instruction->details.sib.isDecoded);
            operand->mem.index = 
                ZydisRegisterEncode(vidxRegisterClass ? vidxRegisterClass : ZYDIS_REGCLASS_GPR32, 
                    ZydisCalcRegisterId(context, instruction, 
                        vidxRegisterClass ? ZYDIS_REG_ENCODING_VIDX : ZYDIS_REG_ENCODING_INDEX, 
                        vidxRegisterClass ? vidxRegisterClass : ZYDIS_REGCLASS_GPR32));
            operand->mem.scale = (1 << instruction->details.sib.scale) & ~1;
            if (operand->mem.index == ZYDIS_REGISTER_ESP)  
            {
                operand->mem.index = ZYDIS_REGISTER_NONE;
                operand->mem.scale = 0;
            } 
            if (operand->mem.base == ZYDIS_REGISTER_EBP)
            {
                if (instruction->details.modrm.mod == 0)
                {
                    operand->mem.base = ZYDIS_REGISTER_NONE;
                } 
                displacementSize = (instruction->details.modrm.mod == 1) ? 8 : 32;
            }
        } else
        {
            operand->mem.index = ZYDIS_REGISTER_NONE;
            operand->mem.scale = 0;    
        }
        break;
    }
    case 64:
    {
        operand->mem.base = ZYDIS_REGISTER_RAX + ZydisCalcRegisterId(context, instruction, 
            ZYDIS_REG_ENCODING_BASE, ZYDIS_REGCLASS_GPR64);
        switch (instruction->details.modrm.mod)
        {
        case 0:
            if (modrm_rm == 5)
            {
                if (context->decoder->machineMode == ZYDIS_MACHINE_MODE_LONG_64)
                {
                    operand->mem.base = ZYDIS_REGISTER_RIP;
                } else
                {
                    operand->mem.base = ZYDIS_REGISTER_NONE;
                }
                displacementSize = 32;
            }
            break;
        case 1:
            displacementSize = 8;
            break;
        case 2:
            displacementSize = 32;
            break;
        default:
            ZYDIS_UNREACHABLE;
        }
        if ((modrm_rm & 0x07) == 4)
        {
            ZYDIS_ASSERT(instruction->details.sib.isDecoded);
            operand->mem.index = 
                ZydisRegisterEncode(vidxRegisterClass ? vidxRegisterClass : ZYDIS_REGCLASS_GPR64, 
                    ZydisCalcRegisterId(context, instruction, 
                        vidxRegisterClass ? ZYDIS_REG_ENCODING_VIDX : ZYDIS_REG_ENCODING_INDEX, 
                        vidxRegisterClass ? vidxRegisterClass : ZYDIS_REGCLASS_GPR64));
            operand->mem.scale = (1 << instruction->details.sib.scale) & ~1;
            if (operand->mem.index == ZYDIS_REGISTER_RSP)  
            {
                operand->mem.index = ZYDIS_REGISTER_NONE;
                operand->mem.scale = 0;
            } 
            if ((operand->mem.base == ZYDIS_REGISTER_RBP) || 
                (operand->mem.base == ZYDIS_REGISTER_R13))
            {
                if (instruction->details.modrm.mod == 0)
                {
                    operand->mem.base = ZYDIS_REGISTER_NONE;
                } 
                displacementSize = (instruction->details.modrm.mod == 1) ? 8 : 32;
            }
        } else
        {
            operand->mem.index = ZYDIS_REGISTER_NONE;
            operand->mem.scale = 0;    
        }
        break;
    }
    default:
        ZYDIS_UNREACHABLE;
    }
    if (displacementSize)
    {
        ZYDIS_ASSERT(instruction->details.disp.size == displacementSize);
        operand->mem.disp.hasDisplacement = ZYDIS_TRUE;
        operand->mem.disp.value = instruction->details.disp.value;
    }
    return ZYDIS_STATUS_SUCCESS;
}

/**
 * @brief   Decodes an implicit register operand.
 *
 * @param   context         A pointer to the @c ZydisDecoderContext instance.
 * @param   instruction     A pointer to the @c ZydisDecodedInstruction struct.
 * @param   operand         A pointer to the @c ZydisDecodedOperand struct.
 * @param   definition      A pointer to the @c ZydisOperandDefinition struct.
 */
static void ZydisDecodeOperandImplicitRegister(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, ZydisDecodedOperand* operand, 
    const ZydisOperandDefinition* definition)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(operand);
    ZYDIS_ASSERT(definition);  
    
    operand->type = ZYDIS_OPERAND_TYPE_REGISTER;

    switch (definition->op.reg.type)
    {
    case ZYDIS_IMPLREG_TYPE_STATIC:
        operand->reg = definition->op.reg.reg.reg;
        break;
    case ZYDIS_IMPLREG_TYPE_GPR_OSZ:
    {
        static const ZydisRegisterClass lookup[3] = 
        {
            ZYDIS_REGCLASS_GPR16,
            ZYDIS_REGCLASS_GPR32,
            ZYDIS_REGCLASS_GPR64
        };
        operand->reg = ZydisRegisterEncode(lookup[context->eoszIndex], definition->op.reg.reg.id);
        break;
    }
    case ZYDIS_IMPLREG_TYPE_GPR_ASZ:
        switch (instruction->addressWidth)
        {
        case 16:
            operand->reg = ZydisRegisterEncode(ZYDIS_REGCLASS_GPR16, definition->op.reg.reg.id);
            break;
        case 32:
            operand->reg = ZydisRegisterEncode(ZYDIS_REGCLASS_GPR32, definition->op.reg.reg.id);
            break;
        case 64:
            operand->reg = ZydisRegisterEncode(ZYDIS_REGCLASS_GPR64, definition->op.reg.reg.id);
            break;
        default:
            ZYDIS_UNREACHABLE;
        }
        break;
    case ZYDIS_IMPLREG_TYPE_GPR_SSZ:
        switch (context->decoder->addressWidth)
        {
        case 16:
            operand->reg = ZydisRegisterEncode(ZYDIS_REGCLASS_GPR16, definition->op.reg.reg.id);
            break;
        case 32:
            operand->reg = ZydisRegisterEncode(ZYDIS_REGCLASS_GPR32, definition->op.reg.reg.id);
            break;
        case 64:
            operand->reg = ZydisRegisterEncode(ZYDIS_REGCLASS_GPR64, definition->op.reg.reg.id);
            break;
        default:
            ZYDIS_UNREACHABLE;
        }
        break;
    case ZYDIS_IMPLREG_TYPE_IP_ASZ:
        switch (instruction->addressWidth)
        {
        case 16:
            operand->reg = ZYDIS_REGISTER_IP;
            break;
        case 32:
            operand->reg = ZYDIS_REGISTER_EIP;
            break;
        case 64:
            operand->reg = ZYDIS_REGISTER_RIP;
            break;
        default:
            ZYDIS_UNREACHABLE;
        }
        break;
    case ZYDIS_IMPLREG_TYPE_IP_SSZ:
        switch (context->decoder->addressWidth)
        {
        case 16:
            operand->reg = ZYDIS_REGISTER_EIP;
            break;
        case 32:
            operand->reg = ZYDIS_REGISTER_EIP;
            break;
        case 64:
            operand->reg = ZYDIS_REGISTER_RIP;
            break;
        default:
            ZYDIS_UNREACHABLE;
        }
        break;
    case ZYDIS_IMPLREG_TYPE_FLAGS_SSZ:
        switch (context->decoder->addressWidth)
        {
        case 16:
            operand->reg = ZYDIS_REGISTER_FLAGS;
            break;
        case 32:
            operand->reg = ZYDIS_REGISTER_EFLAGS;
            break;
        case 64:
            operand->reg = ZYDIS_REGISTER_RFLAGS;
            break;
        default:
            ZYDIS_UNREACHABLE;
        }
        break;
    default:
        ZYDIS_UNREACHABLE;
    }
}

/**
 * @brief   Decodes an implicit memory operand.
 *
 * @param   context         A pointer to the @c ZydisDecoderContext instance.
 * @param   operand         A pointer to the @c ZydisDecodedOperand struct.
 * @param   definition      A pointer to the @c ZydisOperandDefinition struct.
 */
static void ZydisDecodeOperandImplicitMemory(ZydisDecoderContext* context, 
    ZydisDecodedOperand* operand, const ZydisOperandDefinition* definition)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(operand);
    ZYDIS_ASSERT(definition);

    static const ZydisRegisterClass lookup[3] = 
    {
        ZYDIS_REGCLASS_GPR16,
        ZYDIS_REGCLASS_GPR32,
        ZYDIS_REGCLASS_GPR64
    };

    operand->type = ZYDIS_OPERAND_TYPE_MEMORY;

    // TODO: Base action
    switch (definition->op.mem.base)
    {
    case ZYDIS_IMPLMEM_BASE_ABX:
        operand->mem.base = ZydisRegisterEncode(lookup[context->easzIndex], 3);
        break;
    case ZYDIS_IMPLMEM_BASE_ABP:
        operand->mem.base = ZydisRegisterEncode(lookup[context->easzIndex], 5);
        break;
    case ZYDIS_IMPLMEM_BASE_ASI:
        operand->mem.base = ZydisRegisterEncode(lookup[context->easzIndex], 6);
        break;
    case ZYDIS_IMPLMEM_BASE_ADI:
        operand->mem.base = ZydisRegisterEncode(lookup[context->easzIndex], 7);
        break;    
    default:
        ZYDIS_UNREACHABLE;
    }

    if (definition->op.mem.seg)
    {
        operand->mem.segment = 
            ZydisRegisterEncode(ZYDIS_REGCLASS_SEGMENT, definition->op.mem.seg - 1);
        ZYDIS_ASSERT(operand->mem.segment);
    }
}

/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Decodes the instruction operands.
 *          
 * @param   context     A pointer to the @c ZydisDecoderContext struct.
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct.
 * @param   definition  A pointer to the @c ZydisInstructionDefinition struct.
 * 
 * @return  A zydis status code.
 */
static ZydisStatus ZydisDecodeOperands(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, const ZydisInstructionDefinition* definition)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(definition);

    uint8_t immId = 0;
    const ZydisOperandDefinition* operand;
    instruction->operandCount = ZydisGetOperandDefinitions(definition, &operand);

    ZYDIS_ASSERT(instruction->operandCount < ZYDIS_ARRAY_SIZE(instruction->operands));

    for (uint8_t i = 0; i < instruction->operandCount; ++i)
    {
        instruction->operands[i].id = i;
        instruction->operands[i].visibility = operand->visibility;
        instruction->operands[i].action = operand->action;

        // Implicit operands
        switch (operand->type)
        {
        case ZYDIS_SEMANTIC_OPTYPE_IMPLICIT_REG:
            ZydisDecodeOperandImplicitRegister(
                context, instruction, &instruction->operands[i], operand);
            break;
        case ZYDIS_SEMANTIC_OPTYPE_IMPLICIT_MEM:
            ZydisDecodeOperandImplicitMemory(context, &instruction->operands[i], operand);
            break;
        case ZYDIS_SEMANTIC_OPTYPE_IMPLICIT_IMM1:
            instruction->operands[i].type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
            instruction->operands[i].size = 8;
            instruction->operands[i].imm.value.u = 1;
            instruction->operands[i].imm.isSigned = ZYDIS_FALSE;
            instruction->operands[i].imm.isRelative = ZYDIS_FALSE;
            break;
        default:
            break;
        }
        if (instruction->operands[i].type)
        {
            goto FinalizeOperand;
        }

        // Register operands
        ZydisRegisterClass registerClass = ZYDIS_REGCLASS_INVALID;
        switch (operand->type)
        {
        case ZYDIS_SEMANTIC_OPTYPE_GPR8:
            registerClass = ZYDIS_REGCLASS_GPR8;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_GPR16:
            registerClass = ZYDIS_REGCLASS_GPR16;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_GPR32:
            registerClass = ZYDIS_REGCLASS_GPR32;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_GPR64:
            registerClass = ZYDIS_REGCLASS_GPR64;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_GPR16_32_64:
            registerClass = 
                (instruction->operandSize == 16) ? ZYDIS_REGCLASS_GPR16 : (
                (instruction->operandSize == 32) ? ZYDIS_REGCLASS_GPR32 : ZYDIS_REGCLASS_GPR64);
            break; 
        case ZYDIS_SEMANTIC_OPTYPE_GPR32_32_64:
            registerClass = 
                (instruction->operandSize == 16) ? ZYDIS_REGCLASS_GPR32 : (
                (instruction->operandSize == 32) ? ZYDIS_REGCLASS_GPR32: ZYDIS_REGCLASS_GPR64);
            break; 
        case ZYDIS_SEMANTIC_OPTYPE_GPR16_32_32:
            registerClass = 
                (instruction->operandSize == 16) ? ZYDIS_REGCLASS_GPR16 : (
                (instruction->operandSize == 32) ? ZYDIS_REGCLASS_GPR32 : ZYDIS_REGCLASS_GPR32);
            break;  
        case ZYDIS_SEMANTIC_OPTYPE_FPR:
            registerClass = ZYDIS_REGCLASS_X87;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_MMX:
            registerClass = ZYDIS_REGCLASS_MMX;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_XMM:
            registerClass = ZYDIS_REGCLASS_XMM;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_YMM:
            registerClass = ZYDIS_REGCLASS_YMM;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_ZMM:
            registerClass = ZYDIS_REGCLASS_ZMM;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_BND:
            registerClass = ZYDIS_REGCLASS_BOUND;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_SREG:
            registerClass = ZYDIS_REGCLASS_SEGMENT;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_CR:
            registerClass = ZYDIS_REGCLASS_CONTROL;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_DR:
            registerClass = ZYDIS_REGCLASS_DEBUG;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_MASK:
            registerClass = ZYDIS_REGCLASS_MASK;
            break;
        default:
            break;
        }
        if (registerClass)
        {
            instruction->operands[i].encoding = operand->op.encoding;
            switch (operand->op.encoding)
            {
            case ZYDIS_OPERAND_ENCODING_MODRM_REG:
                ZYDIS_CHECK(
                    ZydisDecodeOperandRegister(
                        instruction, &instruction->operands[i], registerClass, 
                    ZydisCalcRegisterId(
                        context, instruction, ZYDIS_REG_ENCODING_REG, registerClass)));
                break;
            case ZYDIS_OPERAND_ENCODING_MODRM_RM:
                ZYDIS_CHECK(
                    ZydisDecodeOperandRegister(
                        instruction, &instruction->operands[i], registerClass, 
                    ZydisCalcRegisterId(
                        context, instruction, ZYDIS_REG_ENCODING_RM, registerClass)));
                break;
            case ZYDIS_OPERAND_ENCODING_OPCODE:
                ZYDIS_CHECK(
                    ZydisDecodeOperandRegister(
                        instruction, &instruction->operands[i], registerClass, 
                    ZydisCalcRegisterId(
                        context, instruction, ZYDIS_REG_ENCODING_OPCODE, registerClass)));
                break;
            case ZYDIS_OPERAND_ENCODING_NDSNDD:
                ZYDIS_CHECK(
                    ZydisDecodeOperandRegister(
                        instruction, &instruction->operands[i], registerClass, 
                    ZydisCalcRegisterId(
                        context, instruction, ZYDIS_REG_ENCODING_NDSNDD, registerClass)));
                break;        
            case ZYDIS_OPERAND_ENCODING_MASK:
                ZYDIS_CHECK(
                    ZydisDecodeOperandRegister(
                        instruction, &instruction->operands[i], registerClass, 
                    ZydisCalcRegisterId(
                        context, instruction, ZYDIS_REG_ENCODING_MASK, registerClass)));
                break;
            case ZYDIS_OPERAND_ENCODING_IS4:
                ZYDIS_CHECK(
                    ZydisDecodeOperandRegister(
                        instruction, &instruction->operands[i], registerClass, 
                    ZydisCalcRegisterId(
                        context, instruction, ZYDIS_REG_ENCODING_IS4, registerClass)));
                break;
            default:
                ZYDIS_UNREACHABLE;
            }
           
            goto FinalizeOperand;
        }

        // Memory operands
        switch (operand->type)
        {
        case ZYDIS_SEMANTIC_OPTYPE_MEM:
            ZYDIS_CHECK(
                ZydisDecodeOperandMemory(
                    context, instruction, &instruction->operands[i], ZYDIS_REGISTER_NONE));
            break;
        case ZYDIS_SEMANTIC_OPTYPE_MEM_VSIBX:
            ZYDIS_CHECK(
                ZydisDecodeOperandMemory(
                    context, instruction, &instruction->operands[i], ZYDIS_REGCLASS_XMM));
            break;
        case ZYDIS_SEMANTIC_OPTYPE_MEM_VSIBY:
            ZYDIS_CHECK(
                ZydisDecodeOperandMemory(
                    context, instruction, &instruction->operands[i], ZYDIS_REGCLASS_YMM));
            break;
        case ZYDIS_SEMANTIC_OPTYPE_MEM_VSIBZ: 
            ZYDIS_CHECK(
                ZydisDecodeOperandMemory(
                    context, instruction, &instruction->operands[i], ZYDIS_REGCLASS_ZMM));
            break;
        case ZYDIS_SEMANTIC_OPTYPE_PTR:        
            ZYDIS_ASSERT((instruction->details.imm[0].size == 16) || 
                         (instruction->details.imm[0].size == 32));
            ZYDIS_ASSERT( instruction->details.imm[1].size == 16);
            instruction->operands[i].type = ZYDIS_OPERAND_TYPE_POINTER;
            instruction->operands[i].ptr.offset  = (uint32_t)instruction->details.imm[0].value.u;
            instruction->operands[i].ptr.segment = (uint16_t)instruction->details.imm[1].value.u;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_AGEN:
            instruction->operands[i].action = ZYDIS_OPERAND_ACTION_INVALID;
            instruction->operands[i].mem.isAddressGenOnly = ZYDIS_TRUE;
            ZYDIS_CHECK(
                ZydisDecodeOperandMemory(
                    context, instruction, &instruction->operands[i], ZYDIS_REGISTER_NONE)); 
            break;
        case ZYDIS_SEMANTIC_OPTYPE_MOFFS:
            ZYDIS_ASSERT(instruction->details.disp.size);
            instruction->operands[i].type = ZYDIS_OPERAND_TYPE_MEMORY;
            instruction->operands[i].mem.disp.hasDisplacement = ZYDIS_TRUE;
            instruction->operands[i].mem.disp.value = instruction->details.disp.value;
            break;
        default:
            break;
        }
        if (instruction->operands[i].type)
        {
            // Handle compressed 8-bit displacement
            if (((instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX) ||
                 (instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_MVEX)) &&
                 (instruction->details.disp.size == 8))
            {
                instruction->operands[i].mem.disp.value *= instruction->avx.compressedDisp8Scale;
            }

            goto FinalizeOperand;
        }

        // Immediate operands
        switch (operand->type)
        {
        case ZYDIS_SEMANTIC_OPTYPE_REL:
            ZYDIS_ASSERT(instruction->details.imm[immId].isRelative);
        case ZYDIS_SEMANTIC_OPTYPE_IMM:
            ZYDIS_ASSERT((immId == 0) || (immId == 1));
            instruction->operands[i].type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
            instruction->operands[i].size = operand->size[context->eoszIndex] * 8;
            if (operand->op.encoding == ZYDIS_OPERAND_ENCODING_IS4)
            {
                // The upper half of the 8-bit immediate is used to encode a register specifier
                ZYDIS_ASSERT(instruction->details.imm[immId].size == 8);
                instruction->operands[i].imm.value.u = 
                    (uint8_t)instruction->details.imm[immId].value.u & 0x0F;   
            } else
            {
                instruction->operands[i].imm.value.u = instruction->details.imm[immId].value.u;
            }
            instruction->operands[i].imm.isSigned = instruction->details.imm[immId].isSigned;
            instruction->operands[i].imm.isRelative = instruction->details.imm[immId].isRelative;
            ++immId;
            break;
        default:
            break;
        }
        ZYDIS_ASSERT(instruction->operands[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE);

FinalizeOperand:
        // Set segment-register for memory operands
        if (instruction->operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY)
        {
            if (instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT_CS)
            {
                instruction->operands[i].mem.segment = ZYDIS_REGISTER_CS;    
            } else
            if (instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT_SS)
            {
                instruction->operands[i].mem.segment = ZYDIS_REGISTER_SS;    
            } else
            if (instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT_DS)
            {
                instruction->operands[i].mem.segment = ZYDIS_REGISTER_DS;    
            } else
            if (instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT_ES)
            {
                instruction->operands[i].mem.segment = ZYDIS_REGISTER_ES;    
            } else
            if (instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT_FS)
            {
                instruction->operands[i].mem.segment = ZYDIS_REGISTER_FS;    
            } else
            if (instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT_GS)
            {
                instruction->operands[i].mem.segment = ZYDIS_REGISTER_GS;    
            } else
            {
                if ((instruction->operands[i].mem.base == ZYDIS_REGISTER_RSP) ||
                    (instruction->operands[i].mem.base == ZYDIS_REGISTER_RBP) || 
                    (instruction->operands[i].mem.base == ZYDIS_REGISTER_ESP) ||
                    (instruction->operands[i].mem.base == ZYDIS_REGISTER_EBP) ||
                    (instruction->operands[i].mem.base == ZYDIS_REGISTER_SP)  ||
                    (instruction->operands[i].mem.base == ZYDIS_REGISTER_BP))
                {
                    instruction->operands[i].mem.segment = ZYDIS_REGISTER_SS;
                } else
                {
                    instruction->operands[i].mem.segment = ZYDIS_REGISTER_DS;
                };
            }
        }

        ZydisSetOperandSizeAndElementInfo(context, instruction, &instruction->operands[i], operand);
        ++operand;
    }

    // Fix operand-action for EVEX instructions with merge-mask
    if (((instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX) || 
         (instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_MVEX)) && 
        (instruction->avx.maskMode == ZYDIS_MASK_MODE_MERGE) &&
        (instruction->operandCount >= 3) &&
        (instruction->operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER) &&
        (instruction->operands[1].reg >= ZYDIS_REGISTER_K1) &&
        (instruction->operands[1].reg <= ZYDIS_REGISTER_K7))
    {
        switch (instruction->operands[0].type)
        {
        case ZYDIS_OPERAND_TYPE_REGISTER:
            ZYDIS_ASSERT((instruction->operands[0].action == ZYDIS_OPERAND_ACTION_WRITE) || 
                         (instruction->operands[0].action == ZYDIS_OPERAND_ACTION_READWRITE));
            instruction->operands[0].action = ZYDIS_OPERAND_ACTION_READ_CONDWRITE;
            break;
        case ZYDIS_OPERAND_TYPE_MEMORY:
            switch (instruction->operands[0].action)
            {
            case ZYDIS_OPERAND_ACTION_READ:
                break;
            case ZYDIS_OPERAND_ACTION_WRITE:
                instruction->operands[0].action = ZYDIS_OPERAND_ACTION_CONDWRITE;
                break;
            case ZYDIS_OPERAND_ACTION_READWRITE:
                instruction->operands[0].action = ZYDIS_OPERAND_ACTION_READ_CONDWRITE;
                break;
            default:
                ZYDIS_UNREACHABLE;
            }
            break;
        default:
            break;
        }
    }

    return ZYDIS_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Sets prefix-related attributes for the given instruction.
 * 
 * @param   context     A pointer to the @c ZydisDecoderContext struct.
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct.
 * @param   definition  A pointer to the @c ZydisInstructionDefinition struct.
 */
static void ZydisSetPrefixRelatedAttributes(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, const ZydisInstructionDefinition* definition)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(definition);

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_DEFAULT:
    {
        const ZydisInstructionDefinitionDEFAULT* def = 
            (const ZydisInstructionDefinitionDEFAULT*)definition;

        if (def->acceptsLock)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_LOCK;
            if (instruction->details.prefixes.hasF0)
            {
                instruction->attributes |= ZYDIS_ATTRIB_HAS_LOCK;
            }
        }
        if (def->acceptsREP)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_REP;
        }
        if (def->acceptsREPEREPZ)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_REPE;
        }
        if (def->acceptsREPNEREPNZ)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_REPNE;
        }
        if (def->acceptsBOUND)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_BOUND;    
        }
        if (def->acceptsXACQUIRE)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_XACQUIRE;    
        }
        if (def->acceptsXRELEASE)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_XRELEASE;    
        }
        if (def->acceptsHLEWithoutLock)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_HLE_WITHOUT_LOCK;
        }

        switch (context->mandatoryCandidate)
        {
        case 0xF2:
            if (instruction->attributes & ZYDIS_ATTRIB_ACCEPTS_REPNE)
            {
                instruction->attributes |= ZYDIS_ATTRIB_HAS_REPNE;
                break;
            }
            if (instruction->attributes & ZYDIS_ATTRIB_ACCEPTS_XACQUIRE)
            {
                if ((instruction->attributes & ZYDIS_ATTRIB_HAS_LOCK) || 
                    (def->acceptsHLEWithoutLock))
                {
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_XACQUIRE;
                    break;
                }
            }
            if (instruction->attributes & ZYDIS_ATTRIB_ACCEPTS_BOUND)
            {
                instruction->attributes |= ZYDIS_ATTRIB_HAS_BOUND;
                break;
            }   
            break;
        case 0xF3:
            if (instruction->attributes & ZYDIS_ATTRIB_ACCEPTS_REP)
            {
                instruction->attributes |= ZYDIS_ATTRIB_HAS_REP;
                break;
            }
            if (instruction->attributes & ZYDIS_ATTRIB_ACCEPTS_REPE)
            {
                instruction->attributes |= ZYDIS_ATTRIB_HAS_REPE;
                break;
            }
            if (instruction->attributes & ZYDIS_ATTRIB_ACCEPTS_XRELEASE)
            {
                if ((instruction->attributes & ZYDIS_ATTRIB_HAS_LOCK) || 
                    (def->acceptsHLEWithoutLock))
                {
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_XRELEASE;
                    break;
                }
            }
            break;
        default:
            break;
        }

        if (def->acceptsBranchHints)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_BRANCH_HINTS;
            switch (context->lastSegmentPrefix)
            {
            case 0x2E:
                instruction->attributes |= ZYDIS_ATTRIB_HAS_BRANCH_NOT_TAKEN;
                break;
            case 0x3E:
                instruction->attributes |= ZYDIS_ATTRIB_HAS_BRANCH_TAKEN;
                break;
            default:
                break;
            }
        } else
        {
            if (def->acceptsSegment)
            {
                instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_SEGMENT;
            }
            if (context->lastSegmentPrefix && def->acceptsSegment)
            {
                switch (context->lastSegmentPrefix)
                {
                case 0x2E: 
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_CS;
                    break;
                case 0x36:
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_SS;
                    break;
                case 0x3E: 
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_DS;
                    break;
                case 0x26: 
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_ES;
                    break;
                case 0x64:
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_FS;
                    break;
                case 0x65: 
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_GS;
                    break;
                default:
                    ZYDIS_UNREACHABLE;
                }
            }
        }

        break;
    }
    case ZYDIS_INSTRUCTION_ENCODING_3DNOW:
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
        if (context->lastSegmentPrefix)
        {
            switch (context->lastSegmentPrefix)
            {
            case 0x2E: 
                instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_CS;
                break;
            case 0x36:
                instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_SS;
                break;
            case 0x3E: 
                instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_DS;
                break;
            case 0x26: 
                instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_ES;
                break;
            case 0x64:
                instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_FS;
                break;
            case 0x65: 
                instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_GS;
                break;
            default:
                ZYDIS_UNREACHABLE;
            }
        }
        break;
    default:
        ZYDIS_UNREACHABLE;
    }
}

/**
 * @brief   Sets AVX-specific information for the given instruction.
 * 
 * @param   context     A pointer to the @c ZydisDecoderContext struct.
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct.
 * @param   definition  A pointer to the @c ZydisInstructionDefinition struct.
 * 
 * Information set for XOP:
 * - Vector Length
 * 
 * Information set for VEX:
 * - Vector length
 * - Static broadcast-factor
 * 
 * Information set for EVEX:
 * - Vector length
 * - Broadcast-factor (static and dynamic)
 * - Rounding-mode and SAE
 * - Mask mode
 * - Compressed 8-bit displacement scale-factor
 * 
 * Information set for MVEX:
 * - Vector length
 * - Broadcast-factor (static and dynamic)
 * - Rounding-mode and SAE
 * - Swizzle- and conversion-mode
 * - Mask mode
 * - Eviction hint
 * - Compressed 8-bit displacement scale-factor
 */
static void ZydisSetAVXInformation(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, const ZydisInstructionDefinition* definition)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(definition);

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
    {
        // Vector length
        static const ZydisVectorLength lookup[2] = 
        { 
            ZYDIS_VECTOR_LENGTH_128, 
            ZYDIS_VECTOR_LENGTH_256
        };
        ZYDIS_ASSERT(context->cache.LL < ZYDIS_ARRAY_SIZE(lookup));
        instruction->avx.vectorLength = lookup[context->cache.LL];
        break;
    }
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
    {
        // Vector length  
        static const ZydisVectorLength lookup[2] = 
        { 
            ZYDIS_VECTOR_LENGTH_128, 
            ZYDIS_VECTOR_LENGTH_256 
        };
        ZYDIS_ASSERT(context->cache.LL < ZYDIS_ARRAY_SIZE(lookup));
        instruction->avx.vectorLength = lookup[context->cache.LL];

        // Static broadcast-factor
        const ZydisInstructionDefinitionVEX* def = 
            (const ZydisInstructionDefinitionVEX*)definition;
        if (def->broadcast)
        {
            instruction->avx.broadcast.isStatic = ZYDIS_TRUE;
            switch (def->broadcast)
            {
            case ZYDIS_VEX_STATIC_BROADCAST_1_TO_2:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_2;
                break;
            case ZYDIS_VEX_STATIC_BROADCAST_1_TO_4:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_4;
                break;
            case ZYDIS_VEX_STATIC_BROADCAST_1_TO_8:  
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_8;
                break;
            case ZYDIS_VEX_STATIC_BROADCAST_1_TO_16:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_16;
                break;
            case ZYDIS_VEX_STATIC_BROADCAST_1_TO_32:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_32;
                break;
            case ZYDIS_VEX_STATIC_BROADCAST_2_TO_4:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_2_TO_4;
                break;
            default:
                ZYDIS_UNREACHABLE;
            }
        }
        break;
    }
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
    {
        const ZydisInstructionDefinitionEVEX* def = 
            (const ZydisInstructionDefinitionEVEX*)definition;
    
        // Vector length
        uint8_t vectorLength = vectorLength = context->cache.LL;;
        if (def->vectorLength)
        {
            vectorLength = def->vectorLength - 1;
        }   
        static const ZydisVectorLength lookup[3] = 
        { 
            ZYDIS_VECTOR_LENGTH_128, 
            ZYDIS_VECTOR_LENGTH_256, 
            ZYDIS_VECTOR_LENGTH_512 
        };
        ZYDIS_ASSERT(vectorLength < ZYDIS_ARRAY_SIZE(lookup));
        instruction->avx.vectorLength = lookup[vectorLength];

        context->evex.tupleType = def->tupleType;
        if (def->tupleType)
        {
            ZYDIS_ASSERT(instruction->details.modrm.mod != 3);
            ZYDIS_ASSERT(def->elementSize);

            // Element size
            switch (def->elementSize)
            {
            case ZYDIS_IELEMENT_SIZE_8:
                context->evex.elementSize = 8;
                break;
            case ZYDIS_IELEMENT_SIZE_16:
                context->evex.elementSize = 16;
                break;
            case ZYDIS_IELEMENT_SIZE_32:
                context->evex.elementSize = 32;
                break;
            case ZYDIS_IELEMENT_SIZE_64:
                context->evex.elementSize = 64;
                break;
            default:
                ZYDIS_UNREACHABLE;
            }

            // Compressed disp8 scale and broadcast-factor
            switch (def->tupleType)
            {
            case ZYDIS_TUPLETYPE_FV:
                switch (instruction->details.evex.b)
                {
                case 0:
                    switch (instruction->avx.vectorLength)
                    {
                    case 128:
                        instruction->avx.compressedDisp8Scale = 16;
                        break;
                    case 256:
                        instruction->avx.compressedDisp8Scale = 32;
                        break;
                    case 512:
                        instruction->avx.compressedDisp8Scale = 64;
                        break;
                    default:
                        ZYDIS_UNREACHABLE;
                    }
                    break;
                case 1:
                    ZYDIS_ASSERT(def->functionality == ZYDIS_EVEX_FUNC_BC);
                    switch (context->cache.W)
                    {
                    case 0:
                        ZYDIS_ASSERT(context->evex.elementSize == 32);
                        instruction->avx.compressedDisp8Scale = 4;
                        switch (instruction->avx.vectorLength)
                        {
                        case 128:
                            instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_4;
                            break;
                        case 256:
                            instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_8;
                            break;
                        case 512:
                            instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_16;
                            break;
                        default:
                            ZYDIS_UNREACHABLE;
                        }
                        break;
                    case 1:
                        ZYDIS_ASSERT(context->evex.elementSize == 64);
                        instruction->avx.compressedDisp8Scale = 8;
                        switch (instruction->avx.vectorLength)
                        {
                        case 128:
                            instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_2;
                            break;
                        case 256:
                            instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_4;
                            break;
                        case 512:
                            instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_8;
                            break;
                        default:
                            ZYDIS_UNREACHABLE;
                        }
                        break;
                    default:
                        ZYDIS_UNREACHABLE;
                    }
                    break;
                default:
                    ZYDIS_UNREACHABLE;
                }
                break;
            case ZYDIS_TUPLETYPE_HV:
                ZYDIS_ASSERT(context->evex.elementSize == 32);
                switch (instruction->details.evex.b)
                {
                case 0:
                    switch (instruction->avx.vectorLength)
                    {
                    case 128:
                        instruction->avx.compressedDisp8Scale = 8;
                        break;
                    case 256:
                        instruction->avx.compressedDisp8Scale = 16;
                        break;
                    case 512:
                        instruction->avx.compressedDisp8Scale = 32;
                        break;
                    default:
                        ZYDIS_UNREACHABLE;
                    }
                    break;
                case 1:
                    instruction->avx.compressedDisp8Scale = 4;
                    switch (instruction->avx.vectorLength)
                    {
                    case 128:
                        instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_2;
                        break;
                    case 256:
                        instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_4;
                        break;
                    case 512:
                        instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_8;
                        break;
                    default:
                        ZYDIS_UNREACHABLE;
                    }
                    break;
                default:
                    ZYDIS_UNREACHABLE;
                }
                break;
            case ZYDIS_TUPLETYPE_FVM:
                switch (instruction->avx.vectorLength)
                {
                case 128:
                    instruction->avx.compressedDisp8Scale = 16;
                    break;
                case 256:
                    instruction->avx.compressedDisp8Scale = 32;
                    break;
                case 512:
                    instruction->avx.compressedDisp8Scale = 64;
                    break;
                default:
                    ZYDIS_UNREACHABLE;
                }
                break;
            case ZYDIS_TUPLETYPE_GSCAT:
                switch (context->cache.W)
                {
                case 0:
                    ZYDIS_ASSERT(context->evex.elementSize == 32);
                    break;
                case 1:
                    ZYDIS_ASSERT(context->evex.elementSize == 64);
                    break;
                default:
                    ZYDIS_UNREACHABLE;
                }
            case ZYDIS_TUPLETYPE_T1S:
                instruction->avx.compressedDisp8Scale = context->evex.elementSize / 8;
                break;
            case ZYDIS_TUPLETYPE_T1F:
                switch (context->evex.elementSize)
                {
                case 32:
                    instruction->avx.compressedDisp8Scale = 4;
                    break;
                case 64:
                    instruction->avx.compressedDisp8Scale = 8;
                    break;
                default:
                    ZYDIS_UNREACHABLE;
                }
                break;
            case ZYDIS_TUPLETYPE_T1_4X:
                ZYDIS_ASSERT(context->evex.elementSize == 32);
                ZYDIS_ASSERT(context->cache.W == 0);
                instruction->avx.compressedDisp8Scale = 16;
                break;
            case ZYDIS_TUPLETYPE_T2:
                switch (context->cache.W)
                {
                case 0:
                    ZYDIS_ASSERT(context->evex.elementSize == 32);
                    instruction->avx.compressedDisp8Scale = 8;
                    break;
                case 1:
                    ZYDIS_ASSERT(context->evex.elementSize == 64);
                    ZYDIS_ASSERT((instruction->avx.vectorLength == 256) || 
                                 (instruction->avx.vectorLength == 512));
                    instruction->avx.compressedDisp8Scale = 16;
                    break;
                default:
                    ZYDIS_UNREACHABLE;
                }
                break;
            case ZYDIS_TUPLETYPE_T4:
                switch (context->cache.W)
                {
                case 0:
                    ZYDIS_ASSERT(context->evex.elementSize == 32);
                    ZYDIS_ASSERT((instruction->avx.vectorLength == 256) || 
                                 (instruction->avx.vectorLength == 512));
                    instruction->avx.compressedDisp8Scale = 16;
                    break;
                case 1:
                    ZYDIS_ASSERT(context->evex.elementSize == 64);
                    ZYDIS_ASSERT(instruction->avx.vectorLength == 512);
                    instruction->avx.compressedDisp8Scale = 32;
                    break;
                default:
                    ZYDIS_UNREACHABLE;
                }
                break;
            case ZYDIS_TUPLETYPE_T8:
                ZYDIS_ASSERT(!context->cache.W);
                ZYDIS_ASSERT(instruction->avx.vectorLength == 512);
                ZYDIS_ASSERT(context->evex.elementSize == 32);
                instruction->avx.compressedDisp8Scale = 32;
                break;
            case ZYDIS_TUPLETYPE_HVM:
                switch (instruction->avx.vectorLength)
                {
                case 128:
                    instruction->avx.compressedDisp8Scale = 8;
                    break;
                case 256:
                    instruction->avx.compressedDisp8Scale = 16;
                    break;
                case 512:
                    instruction->avx.compressedDisp8Scale = 32;
                    break;
                default:
                    ZYDIS_UNREACHABLE;
                }
                break;
            case ZYDIS_TUPLETYPE_QVM:
                switch (instruction->avx.vectorLength)
                {
                case 128:
                    instruction->avx.compressedDisp8Scale = 4;
                    break;
                case 256:
                    instruction->avx.compressedDisp8Scale = 8;
                    break;
                case 512:
                    instruction->avx.compressedDisp8Scale = 16;
                    break;
                default:
                    ZYDIS_UNREACHABLE;
                }
                break;
            case ZYDIS_TUPLETYPE_OVM:
                switch (instruction->avx.vectorLength)
                {
                case 128:
                    instruction->avx.compressedDisp8Scale = 2;
                    break;
                case 256:
                    instruction->avx.compressedDisp8Scale = 4;
                    break;
                case 512:
                    instruction->avx.compressedDisp8Scale = 8;
                    break;
                default:
                    ZYDIS_UNREACHABLE;
                }
                break;
            case ZYDIS_TUPLETYPE_M128:
                instruction->avx.compressedDisp8Scale = 16;
                break;
            case ZYDIS_TUPLETYPE_DUP:
                switch (instruction->avx.vectorLength)
                {
                case 128:
                    instruction->avx.compressedDisp8Scale = 8;
                    break;
                case 256:
                    instruction->avx.compressedDisp8Scale = 32;
                    break;
                case 512:
                    instruction->avx.compressedDisp8Scale = 64;
                    break;
                default:
                    ZYDIS_UNREACHABLE;
                }
                break;
            default:
                ZYDIS_UNREACHABLE;
            }
        } else
        {
            ZYDIS_ASSERT(instruction->details.modrm.mod == 3);
        }

        // Static broadcast-factor
        if (def->broadcast)
        {
            ZYDIS_ASSERT(!instruction->avx.broadcast.mode);
            instruction->avx.broadcast.isStatic = ZYDIS_TRUE;
            switch (def->broadcast)
            {
            case ZYDIS_EVEX_STATIC_BROADCAST_1_TO_2:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_2;
                break;
            case ZYDIS_EVEX_STATIC_BROADCAST_1_TO_4:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_4;
                break;
            case ZYDIS_EVEX_STATIC_BROADCAST_1_TO_8:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_8;
                break;
            case ZYDIS_EVEX_STATIC_BROADCAST_1_TO_16:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_16;
                break;
            case ZYDIS_EVEX_STATIC_BROADCAST_1_TO_32:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_32;
                break;
            case ZYDIS_EVEX_STATIC_BROADCAST_1_TO_64:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_64;
                break;
            case ZYDIS_EVEX_STATIC_BROADCAST_2_TO_4:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_2_TO_4;
                break;
            case ZYDIS_EVEX_STATIC_BROADCAST_2_TO_8:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_2_TO_8;
                break;
            case ZYDIS_EVEX_STATIC_BROADCAST_2_TO_16:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_2_TO_16;
                break;
            case ZYDIS_EVEX_STATIC_BROADCAST_4_TO_8:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_4_TO_8;
                break;
            case ZYDIS_EVEX_STATIC_BROADCAST_4_TO_16:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_4_TO_16;
                break;
            case ZYDIS_EVEX_STATIC_BROADCAST_8_TO_16:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_8_TO_16;
                break;
            default:
                ZYDIS_UNREACHABLE;
            }
        }

        // Rounding mode and SAE
        if (instruction->details.evex.b)
        {
            switch (def->functionality)
            {
            case ZYDIS_EVEX_FUNC_INVALID:
            case ZYDIS_EVEX_FUNC_BC:
                // Noting to do here
                break;
            case ZYDIS_EVEX_FUNC_RC:
                instruction->avx.roundingMode = ZYDIS_ROUNDING_MODE_RN + context->cache.LL;
                // Intentional fallthrough
            case ZYDIS_EVEX_FUNC_SAE:
                instruction->avx.hasSAE = ZYDIS_TRUE;
                break;
            default:
                ZYDIS_UNREACHABLE;
            }
        }

        // Mask mode
        instruction->avx.maskMode = ZYDIS_MASK_MODE_MERGE + instruction->details.evex.z;

        break;
    }
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
    {
        // Vector length
        instruction->avx.vectorLength = ZYDIS_VECTOR_LENGTH_512;

        const ZydisInstructionDefinitionMVEX* def = 
            (const ZydisInstructionDefinitionMVEX*)definition; 

        // Static broadcast-factor
        uint8_t index = def->hasElementGranularity;
        ZYDIS_ASSERT(!index || !def->broadcast);
        if (!index && def->broadcast)
        {
            instruction->avx.broadcast.isStatic = ZYDIS_TRUE;
            switch (def->broadcast)
            {
            case ZYDIS_MVEX_STATIC_BROADCAST_1_TO_8:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_8;
                index = 1;
                break;
            case ZYDIS_MVEX_STATIC_BROADCAST_1_TO_16:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_16;
                index = 1;
                break;
            case ZYDIS_MVEX_STATIC_BROADCAST_4_TO_8:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_4_TO_8;
                index = 2;
                break;
            case ZYDIS_MVEX_STATIC_BROADCAST_4_TO_16: 
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_4_TO_16;
                index = 2;
                break;
            default:
                ZYDIS_UNREACHABLE;
            }
        }

        // Compressed disp8 scale and broadcast-factor
        switch (def->functionality)
        {
        case ZYDIS_MVEX_FUNC_IGNORED:
        case ZYDIS_MVEX_FUNC_INVALID:
        case ZYDIS_MVEX_FUNC_RC:
        case ZYDIS_MVEX_FUNC_SAE:
        case ZYDIS_MVEX_FUNC_SWIZZLE_32:
        case ZYDIS_MVEX_FUNC_SWIZZLE_64:
            // Nothing to do here
            break;
        case ZYDIS_MVEX_FUNC_F_32:
        case ZYDIS_MVEX_FUNC_I_32:
        case ZYDIS_MVEX_FUNC_F_64:
        case ZYDIS_MVEX_FUNC_I_64: 
            instruction->avx.compressedDisp8Scale = 64;
            break;
        case ZYDIS_MVEX_FUNC_SF_32:
        case ZYDIS_MVEX_FUNC_SF_32_BCST:
        case ZYDIS_MVEX_FUNC_SF_32_BCST_4TO16:
        case ZYDIS_MVEX_FUNC_UF_32:
        {    
            static const uint8_t lookup[3][8] = 
            {
                { 64,  4, 16, 32, 16, 16, 32, 32 },
                { 4,   0,  0,  2,  1,  1,  2,  2 },
                { 16,  0,  0,  8,  4,  4,  8,  8 }              
            };
            ZYDIS_ASSERT(instruction->details.mvex.SSS < ZYDIS_ARRAY_SIZE(lookup[index]));
            instruction->avx.compressedDisp8Scale = lookup[index][instruction->details.mvex.SSS];
            break;
        }
        case ZYDIS_MVEX_FUNC_SI_32:
        case ZYDIS_MVEX_FUNC_UI_32:
        case ZYDIS_MVEX_FUNC_SI_32_BCST:
        case ZYDIS_MVEX_FUNC_SI_32_BCST_4TO16:
        {    
            static const uint8_t lookup[3][8] = 
            {
                { 64,  4, 16,  0, 16, 16, 32, 32 },
                {  4,  0,  0,  0,  1,  1,  2,  2 },
                { 16,  0,  0,  0,  4,  4,  8,  8 }              
            };
            ZYDIS_ASSERT(instruction->details.mvex.SSS < ZYDIS_ARRAY_SIZE(lookup[index]));
            instruction->avx.compressedDisp8Scale = lookup[index][instruction->details.mvex.SSS];
            break;
        }
        case ZYDIS_MVEX_FUNC_SF_64:
        case ZYDIS_MVEX_FUNC_UF_64:
        case ZYDIS_MVEX_FUNC_SI_64:
        case ZYDIS_MVEX_FUNC_UI_64:
        {    
            static const uint8_t lookup[3][3] = 
            {
                { 64,  8, 32 },
                {  8,  0,  0 },
                { 32,  0,  0 }               
            };
            ZYDIS_ASSERT(instruction->details.mvex.SSS < ZYDIS_ARRAY_SIZE(lookup[index]));
            instruction->avx.compressedDisp8Scale = lookup[index][instruction->details.mvex.SSS];
            break;
        }
        case ZYDIS_MVEX_FUNC_DF_32:
        case ZYDIS_MVEX_FUNC_DI_32:
        {    
            static const uint8_t lookup[2][8] = 
            {
                { 64,  0,  0, 32, 16, 16, 32, 32 },
                {  4,  0,  0,  2,  1,  1,  2,  2 }
            };
            ZYDIS_ASSERT(instruction->details.mvex.SSS < ZYDIS_ARRAY_SIZE(lookup[index]));
            instruction->avx.compressedDisp8Scale = lookup[index][instruction->details.mvex.SSS];
            break;
        }
        case ZYDIS_MVEX_FUNC_DF_64:
        case ZYDIS_MVEX_FUNC_DI_64:
        {
            static const uint8_t lookup[2][1] = 
            {
                { 64 },
                {  8 }
            };
            ZYDIS_ASSERT(instruction->details.mvex.SSS < ZYDIS_ARRAY_SIZE(lookup[index]));
            instruction->avx.compressedDisp8Scale = lookup[index][instruction->details.mvex.SSS];
            break;        
        }
        default:
            ZYDIS_UNREACHABLE;
        }

        // Rounding mode, sae, swizzle, convert
        context->mvex.functionality = def->functionality;
        switch (def->functionality)
        {
        case ZYDIS_MVEX_FUNC_IGNORED:
        case ZYDIS_MVEX_FUNC_INVALID:
        case ZYDIS_MVEX_FUNC_F_32:
        case ZYDIS_MVEX_FUNC_I_32:
        case ZYDIS_MVEX_FUNC_F_64:
        case ZYDIS_MVEX_FUNC_I_64:
            // Nothing to do here
            break;
        case ZYDIS_MVEX_FUNC_RC:
            instruction->avx.roundingMode = ZYDIS_ROUNDING_MODE_RN + instruction->details.mvex.SSS;
            break;
        case ZYDIS_MVEX_FUNC_SAE:
            if (instruction->details.mvex.SSS >= 4)
            {
                instruction->avx.hasSAE = ZYDIS_TRUE;
            }
            break;
        case ZYDIS_MVEX_FUNC_SWIZZLE_32:
        case ZYDIS_MVEX_FUNC_SWIZZLE_64:
            instruction->avx.swizzleMode = ZYDIS_SWIZZLE_MODE_DCBA + instruction->details.mvex.SSS;
            break;
        case ZYDIS_MVEX_FUNC_SF_32:
        case ZYDIS_MVEX_FUNC_SF_32_BCST:
        case ZYDIS_MVEX_FUNC_SF_32_BCST_4TO16:
            switch (instruction->details.mvex.SSS)
            {
            case 0:
                break;
            case 1:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_16;
                break;
            case 2:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_4_TO_16;
                break;
            case 3:
                instruction->avx.conversionMode = ZYDIS_CONVERSION_MODE_FLOAT16;
                break;
            case 4:
                instruction->avx.conversionMode = ZYDIS_CONVERSION_MODE_UINT8;
                break;
            case 5:
                instruction->avx.conversionMode = ZYDIS_CONVERSION_MODE_SINT8;
                break;
            case 6:
                instruction->avx.conversionMode = ZYDIS_CONVERSION_MODE_UINT16;
                break;
            case 7:
                instruction->avx.conversionMode = ZYDIS_CONVERSION_MODE_SINT16;
                break;
            default:
                ZYDIS_UNREACHABLE;
            }
            break;
        case ZYDIS_MVEX_FUNC_SI_32:
        case ZYDIS_MVEX_FUNC_SI_32_BCST:
        case ZYDIS_MVEX_FUNC_SI_32_BCST_4TO16:
            switch (instruction->details.mvex.SSS)
            {
            case 0:
                break;
            case 1:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_16;
                break;
            case 2:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_4_TO_16;
                break;
            case 4:
                instruction->avx.conversionMode = ZYDIS_CONVERSION_MODE_UINT8;
                break;
            case 5:
                instruction->avx.conversionMode = ZYDIS_CONVERSION_MODE_SINT8;
                break;
            case 6:
                instruction->avx.conversionMode = ZYDIS_CONVERSION_MODE_UINT16;
                break;
            case 7:
                instruction->avx.conversionMode = ZYDIS_CONVERSION_MODE_SINT16;
                break;
            default:
                ZYDIS_UNREACHABLE;
            }
            break;
        case ZYDIS_MVEX_FUNC_SF_64:
        case ZYDIS_MVEX_FUNC_SI_64:
            switch (instruction->details.mvex.SSS)
            {
            case 0:
                break;
            case 1:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_8;
                break;
            case 2:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_4_TO_8;
                break;
            default:
                ZYDIS_UNREACHABLE;
            }
            break;     
        case ZYDIS_MVEX_FUNC_UF_32:
        case ZYDIS_MVEX_FUNC_DF_32:
            switch (instruction->details.mvex.SSS)
            {
            case 0:
                break;
            case 3:
                instruction->avx.conversionMode = ZYDIS_CONVERSION_MODE_FLOAT16;
                break;
            case 4:
                instruction->avx.conversionMode = ZYDIS_CONVERSION_MODE_UINT8;
                break;
            case 5:
                instruction->avx.conversionMode = ZYDIS_CONVERSION_MODE_SINT8;
                break;
            case 6:
                instruction->avx.conversionMode = ZYDIS_CONVERSION_MODE_UINT16;
                break;
            case 7:
                instruction->avx.conversionMode = ZYDIS_CONVERSION_MODE_SINT16;
                break;
            default:
                ZYDIS_UNREACHABLE;
            }
            break;
        case ZYDIS_MVEX_FUNC_UF_64:
        case ZYDIS_MVEX_FUNC_DF_64:
            break;
        case ZYDIS_MVEX_FUNC_UI_32:
        case ZYDIS_MVEX_FUNC_DI_32:
            switch (instruction->details.mvex.SSS)
            {
            case 0:
                break;
            case 4:
                instruction->avx.conversionMode = ZYDIS_CONVERSION_MODE_UINT8;
                break;
            case 5:
                instruction->avx.conversionMode = ZYDIS_CONVERSION_MODE_SINT8;
                break;
            case 6:
                instruction->avx.conversionMode = ZYDIS_CONVERSION_MODE_UINT16;
                break;
            case 7:
                instruction->avx.conversionMode = ZYDIS_CONVERSION_MODE_SINT16;
                break;
            default:
                ZYDIS_UNREACHABLE;
            }
            break;
        case ZYDIS_MVEX_FUNC_UI_64:
        case ZYDIS_MVEX_FUNC_DI_64:
            break;
        default:
            ZYDIS_UNREACHABLE;
        }

        // Eviction hint
        if ((instruction->details.modrm.mod != 3) && instruction->details.mvex.E)
        {
            instruction->avx.hasEvictionHint = ZYDIS_TRUE;
        }

        // Mask mode
        instruction->avx.maskMode = ZYDIS_MASK_MODE_MERGE;

        break;
    }
    default:
        // Nothing to do here
        break;
    }
}

/* ---------------------------------------------------------------------------------------------- */
/* Physical instruction decoding                                                                  */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Collects optional instruction prefixes.
 *
 * @param   context     A pointer to the @c ZydisDecoderContext struct.
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct.
 *
 * @return  A zydis status code.
 *         
 * This function sets the corresponding flag for each prefix and automatically decodes the last
 * REX-prefix (if exists).
 */
static ZydisStatus ZydisCollectOptionalPrefixes(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(instruction->details.prefixes.count == 0);

    ZydisBool done = ZYDIS_FALSE;
    do
    {
        uint8_t prefixByte;
        ZYDIS_CHECK(ZydisInputPeek(context, instruction, &prefixByte));
        switch (prefixByte)
        {
        case 0xF0:
            ++instruction->details.prefixes.hasF0;
            break;
        case 0xF2:
            context->mandatoryCandidate = 0xF2;
            ++instruction->details.prefixes.hasF2;
            break;
        case 0xF3:
            context->mandatoryCandidate = 0xF3;
            ++instruction->details.prefixes.hasF3;
            break;
        case 0x2E: 
            ++instruction->details.prefixes.has2E;
            if ((context->decoder->machineMode != ZYDIS_MACHINE_MODE_LONG_64) ||
               ((context->lastSegmentPrefix != 0x64) && (context->lastSegmentPrefix != 0x65)))
            {
                context->lastSegmentPrefix = 0x2E;
            }
            break;
        case 0x36:
            ++instruction->details.prefixes.has36;
            if ((context->decoder->machineMode != ZYDIS_MACHINE_MODE_LONG_64) ||
               ((context->lastSegmentPrefix != 0x64) && (context->lastSegmentPrefix != 0x65)))
            {
                context->lastSegmentPrefix = 0x36;
            }
            break;
        case 0x3E: 
            ++instruction->details.prefixes.has3E;
            if ((context->decoder->machineMode != ZYDIS_MACHINE_MODE_LONG_64) ||
               ((context->lastSegmentPrefix != 0x64) && (context->lastSegmentPrefix != 0x65)))
            {
                context->lastSegmentPrefix = 0x3E;
            }
            break;
        case 0x26: 
            ++instruction->details.prefixes.has26;
            if ((context->decoder->machineMode != ZYDIS_MACHINE_MODE_LONG_64) ||
               ((context->lastSegmentPrefix != 0x64) && (context->lastSegmentPrefix != 0x65)))
            {
                context->lastSegmentPrefix = 0x26;
            }
            break;
        case 0x64:
            ++instruction->details.prefixes.has64;
            context->lastSegmentPrefix = 0x64;
            break;
        case 0x65: 
            ++instruction->details.prefixes.has65;
            context->lastSegmentPrefix = 0x65;
            break;
        case 0x66:
            if (!context->mandatoryCandidate)
            {
                context->mandatoryCandidate = 0x66;
            }
            ++instruction->details.prefixes.has66;
            instruction->attributes |= ZYDIS_ATTRIB_HAS_OPERANDSIZE;
            break;
        case 0x67:
            ++instruction->details.prefixes.has67;
            instruction->attributes |= ZYDIS_ATTRIB_HAS_ADDRESSSIZE;
            break;
        default:
            if ((context->decoder->machineMode == ZYDIS_MACHINE_MODE_LONG_64) && 
                (prefixByte & 0xF0) == 0x40)
            {
                instruction->details.rex.data[0] = prefixByte; 
            } else
            {
                done = ZYDIS_TRUE;
            }
            break;
        }
        if (!done)
        {
            if ((prefixByte & 0xF0) != 0x40)
            {
                instruction->details.rex.data[0] = 0x00;       
            }
            context->prefixes[instruction->details.prefixes.count] = prefixByte;
            instruction->details.prefixes.data[instruction->details.prefixes.count++] = prefixByte;
            ZydisInputSkip(context, instruction);
        }
    } while (!done);

    if (instruction->details.rex.data[0])
    {
        ZydisDecodeREX(context, instruction, instruction->details.rex.data[0]);
    }

    return ZYDIS_STATUS_SUCCESS;
}

/**
 * @brief   Decodes optional instruction parts like the ModRM byte, the SIB byte and additional
 *          displacements and/or immediate values.
 *          
 * @param   context         A pointer to the @c ZydisDecoderContext struct.
 * @param   instruction     A pointer to the @c ZydisDecodedInstruction struct.
 * @param   optionalParts   A pointer to the @c ZydisInstructionParts struct.
 * 
 * @return  A zydis status code.
 */
static ZydisStatus ZydisDecodeOptionalInstructionParts(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, const ZydisInstructionParts* optionalParts)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(optionalParts);

    if (optionalParts->flags & ZYDIS_INSTRPART_FLAG_HAS_MODRM)
    {
        if (!instruction->details.modrm.isDecoded)
        {
            uint8_t modrmByte;
            ZYDIS_CHECK(ZydisInputNext(context, instruction, &modrmByte));
            ZydisDecodeModRM(instruction, modrmByte);               
        }
        uint8_t hasSIB = 0;
        uint8_t displacementSize = 0;
        if (!(optionalParts->flags & ZYDIS_INSTRPART_FLAG_FORCE_REG_FORM))
        {
            switch (instruction->addressWidth)
            {
            case 16:
                switch (instruction->details.modrm.mod)
                {
                case 0:
                    if (instruction->details.modrm.rm == 6) 
                    {
                        displacementSize = 16;
                    }
                    break;
                case 1:
                    displacementSize = 8;
                    break;
                case 2:
                    displacementSize = 16;
                    break;
                case 3:
                    break;
                default:
                    ZYDIS_UNREACHABLE;
                }
            case 32:
            case 64:
                hasSIB = 
                    (instruction->details.modrm.mod != 3) && (instruction->details.modrm.rm == 4);
                switch (instruction->details.modrm.mod)
                {
                case 0:
                    if (instruction->details.modrm.rm == 5)
                    {
                        if (context->decoder->machineMode == 64)
                        {
                            instruction->attributes |= ZYDIS_ATTRIB_IS_RELATIVE;
                        }
                        displacementSize = 32;
                    }
                    break;
                case 1:
                    displacementSize = 8;
                    break;
                case 2:
                    displacementSize = 32;
                    break;
                case 3:
                    break;
                default:
                    ZYDIS_UNREACHABLE;
                }
                break;
            default:
                ZYDIS_UNREACHABLE;
            }
            if (hasSIB)
            {
                uint8_t sibByte;
                ZYDIS_CHECK(ZydisInputNext(context, instruction, &sibByte)); 
                ZydisDecodeSIB(instruction, sibByte);
                if (instruction->details.sib.base == 5)
                {
                    displacementSize = (instruction->details.modrm.mod == 1) ? 8 : 32;
                }
            }
            if (displacementSize)
            {
                ZYDIS_CHECK(ZydisReadDisplacement(context, instruction, displacementSize));
            } 
        }
    }

    if (optionalParts->flags & ZYDIS_INSTRPART_FLAG_HAS_DISP)
    {
        ZYDIS_CHECK(ZydisReadDisplacement(
            context, instruction, optionalParts->disp.size[context->easzIndex]));    
    }

    if (optionalParts->flags & ZYDIS_INSTRPART_FLAG_HAS_IMM0)
    {
        if (optionalParts->imm[0].isRelative)
        {
            instruction->attributes |= ZYDIS_ATTRIB_IS_RELATIVE;
        }
        ZYDIS_CHECK(ZydisReadImmediate(context, instruction, 0, 
            optionalParts->imm[0].size[context->eoszIndex], 
            optionalParts->imm[0].isSigned, optionalParts->imm[0].isRelative));    
    }

    if (optionalParts->flags & ZYDIS_INSTRPART_FLAG_HAS_IMM1)
    {
        ZYDIS_ASSERT(!(optionalParts->flags & ZYDIS_INSTRPART_FLAG_HAS_DISP));
        ZYDIS_CHECK(ZydisReadImmediate(context, instruction, 1, 
            optionalParts->imm[1].size[context->eoszIndex], 
            optionalParts->imm[1].isSigned, optionalParts->imm[1].isRelative));     
    }

    return ZYDIS_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Sets the effective operand size for the given instruction.
 * 
 * @param   context     A pointer to the @c ZydisDecoderContext struct.
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct.
 * @param   definition  A pointer to the @c ZydisInstructionDefinition struct.
 */
static void ZydisSetEffectiveOperandSize(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, const ZydisInstructionDefinition* definition)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(definition);

    static const uint8_t operandSizeMap[7][8] =
    {
        // Default for most instructions
        {
            16, // 16 __ W0
            32, // 16 66 W0
            32, // 32 __ W0
            16, // 32 66 W0
            32, // 64 __ W0
            16, // 64 66 W0
            64, // 64 __ W1
            64  // 64 66 W1
        },
        // Operand size override 0x66 is ignored
        {
            16, // 16 __ W0
            16, // 16 66 W0
            32, // 32 __ W0
            32, // 32 66 W0
            32, // 64 __ W0
            32, // 64 66 W0
            64, // 64 __ W1
            64  // 64 66 W1
        },
        // REX.W promotes to 32-bit instead of 64-bit
        {
            16, // 16 __ W0
            32, // 16 66 W0
            32, // 32 __ W0
            16, // 32 66 W0
            32, // 64 __ W0
            16, // 64 66 W0
            32, // 64 __ W1
            32  // 64 66 W1
        },
        // Operand size defaults to 64-bit in 64-bit mode
        {
            16, // 16 __ W0
            32, // 16 66 W0
            32, // 32 __ W0
            16, // 32 66 W0
            64, // 64 __ W0
            16, // 64 66 W0
            64, // 64 __ W1
            64  // 64 66 W1
        },
        // Operand size is forced to 64-bit in 64-bit mode
        {
            16, // 16 __ W0
            32, // 16 66 W0
            32, // 32 __ W0
            16, // 32 66 W0
            64, // 64 __ W0
            64, // 64 66 W0
            64, // 64 __ W1
            64  // 64 66 W1
        },
        // Operand size is forced to 32-bit, if no REX.W is present.
        {
            32, // 16 __ W0
            32, // 16 66 W0
            32, // 32 __ W0
            32, // 32 66 W0
            32, // 64 __ W0
            32, // 64 66 W0
            64, // 64 __ W1
            64  // 64 66 W1
        },
        // Operand size is forced to 64-bit in 64-bit mode and forced to 32-bit in all other nmodes.
        // This is used for `mov CR, GPR` and `mov GPR, CR`.
        {
            32, // 16 __ W0
            32, // 16 66 W0
            32, // 32 __ W0
            32, // 32 66 W0
            64, // 64 __ W0
            64, // 64 66 W0
            64, // 64 __ W1
            64  // 64 66 W1
        }
    };
    
    uint8_t index = (instruction->attributes & ZYDIS_ATTRIB_HAS_OPERANDSIZE) ? 1 : 0;
    switch (context->decoder->machineMode)
    {
    case 16:
        index += 0;
        break;
    case 32:
        index += 2;
        break;
    case 64:
        index += 4;
        index += (context->cache.W & 0x01) << 1;
        break;
    default:
        ZYDIS_UNREACHABLE;
    }

    ZYDIS_ASSERT(definition->operandSizeMap < ZYDIS_ARRAY_SIZE(operandSizeMap));
    ZYDIS_ASSERT(index < ZYDIS_ARRAY_SIZE(operandSizeMap[definition->operandSizeMap]));

    instruction->operandSize = operandSizeMap[definition->operandSizeMap][index];
    
    switch (instruction->operandSize)
    {
    case 16:
        context->eoszIndex = 0;
        break;
    case 32:
        context->eoszIndex = 1;
        break;
    case 64:
        context->eoszIndex = 2;
        break;
    default:
        ZYDIS_UNREACHABLE;
    }
}

/**
 * @brief   Sets the effective address width for the given instruction.
 * 
 * @param   context     A pointer to the @c ZydisDecoderContext struct.
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct.
 */
static void ZydisSetEffectiveAddressWidth(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);

    switch (context->decoder->addressWidth)
    {
    case 16:
        instruction->addressWidth = 
            (instruction->attributes & ZYDIS_ATTRIB_HAS_ADDRESSSIZE) ? 32 : 16;
        break;
    case 32:
        instruction->addressWidth = 
            (instruction->attributes & ZYDIS_ATTRIB_HAS_ADDRESSSIZE) ? 16 : 32;
        break;
    case 64:
        instruction->addressWidth = 
            (instruction->attributes & ZYDIS_ATTRIB_HAS_ADDRESSSIZE) ? 32 : 64;
        break;
    default:
        ZYDIS_UNREACHABLE;
    }

    switch (instruction->addressWidth)
    {
    case 16:
        context->easzIndex = 0;
        break;
    case 32:
        context->easzIndex = 1;
        break;
    case 64:
        context->easzIndex = 2;
        break;
    default:
        ZYDIS_UNREACHABLE;
    }
}

/* ---------------------------------------------------------------------------------------------- */

static ZydisStatus ZydisNodeHandlerXOP(ZydisDecodedInstruction* instruction, uint16_t* index)
{
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(index);

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_DEFAULT:
        *index = 0;
        break;
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
        ZYDIS_ASSERT(instruction->details.xop.isDecoded);
        *index = (instruction->details.xop.m_mmmm - 0x08) + 1;
        break;
    default:
        ZYDIS_UNREACHABLE;
    } 
    return ZYDIS_STATUS_SUCCESS;      
}

static ZydisStatus ZydisNodeHandlerVEX(ZydisDecodedInstruction* instruction, uint16_t* index)
{
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(index);

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_DEFAULT:
        *index = 0;
        break;
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
        ZYDIS_ASSERT(instruction->details.vex.isDecoded);
        *index = instruction->details.vex.m_mmmm + (instruction->details.vex.pp << 2) + 1;
        break;
    default:
        ZYDIS_UNREACHABLE;
    }
    return ZYDIS_STATUS_SUCCESS;
}

static ZydisStatus ZydisNodeHandlerEMVEX(ZydisDecodedInstruction* instruction, uint16_t* index)
{
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(index);

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_DEFAULT:
        *index = 0;
        break;
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
        ZYDIS_ASSERT(instruction->details.evex.isDecoded);
        *index = instruction->details.evex.mm + (instruction->details.evex.pp << 2) + 1;
        break;
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
        ZYDIS_ASSERT(instruction->details.mvex.isDecoded);
        *index = instruction->details.mvex.mmmm + (instruction->details.mvex.pp << 2) + 17;
        break;
    default:
        ZYDIS_UNREACHABLE;
    }
    return ZYDIS_STATUS_SUCCESS;
}

static ZydisStatus ZydisNodeHandlerOpcode(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint16_t* index)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(index);

    // Handle possible encoding-prefix and opcode-map changes
    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_DEFAULT:
        ZYDIS_CHECK(ZydisInputNext(context, instruction, &instruction->opcode));
        switch (instruction->opcodeMap)
        {
        case ZYDIS_OPCODE_MAP_DEFAULT:
            switch (instruction->opcode)
            {
            case 0x0F:
                instruction->opcodeMap = ZYDIS_OPCODE_MAP_0F;
                break;
            case 0xC4:
            case 0xC5:
            case 0x62:
            {
                uint8_t nextInput;
                ZYDIS_CHECK(ZydisInputPeek(context, instruction, &nextInput)); 
                if (((nextInput & 0xF0) >= 0xC0) || 
                    (context->decoder->machineMode == ZYDIS_MACHINE_MODE_LONG_64))
                {
                    if (instruction->attributes & ZYDIS_ATTRIB_HAS_REX)
                    {
                        return ZYDIS_STATUS_ILLEGAL_REX;
                    }
                    if (context->mandatoryCandidate)
                    {
                        return ZYDIS_STATUS_ILLEGAL_LEGACY_PFX;
                    }
                    uint8_t prefixBytes[4] = { 0, 0, 0, 0 };
                    prefixBytes[0] = instruction->opcode;
                    switch (instruction->opcode)
                    {
                    case 0xC4:
                        // Read additional 3-byte VEX-prefix data
                        ZYDIS_ASSERT(!instruction->details.vex.isDecoded);
                        ZYDIS_CHECK(ZydisInputNext(context, instruction, &prefixBytes[1]));
                        ZYDIS_CHECK(ZydisInputNext(context, instruction, &prefixBytes[2]));
                        //ZYDIS_CHECK(ZydisInputNextBytes(context, instruction, &prefixBytes[1], 2));
                        break;
                    case 0xC5:
                        // Read additional 2-byte VEX-prefix data
                        ZYDIS_ASSERT(!instruction->details.vex.isDecoded);
                        ZYDIS_CHECK(ZydisInputNext(context, instruction, &prefixBytes[1]));
                        break;
                    case 0x62:
                        // Read additional EVEX/MVEX-prefix data
                        ZYDIS_ASSERT(!instruction->details.evex.isDecoded);
                        ZYDIS_ASSERT(!instruction->details.mvex.isDecoded);
                        ZYDIS_CHECK(ZydisInputNext(context, instruction, &prefixBytes[1]));
                        ZYDIS_CHECK(ZydisInputNext(context, instruction, &prefixBytes[2]));
                        ZYDIS_CHECK(ZydisInputNext(context, instruction, &prefixBytes[3]));
                        //ZYDIS_CHECK(ZydisInputNextBytes(context, instruction, &prefixBytes[1], 3));
                        break;
                    default:
                        ZYDIS_UNREACHABLE;
                    }
                    switch (instruction->opcode)
                    {
                    case 0xC4:
                    case 0xC5:
                        // Decode VEX-prefix
                        instruction->encoding = ZYDIS_INSTRUCTION_ENCODING_VEX;
                        ZYDIS_CHECK(ZydisDecodeVEX(context, instruction, prefixBytes));
                        instruction->opcodeMap = 
                            ZYDIS_OPCODE_MAP_EX0 + instruction->details.vex.m_mmmm;
                        break;
                    case 0x62:
                        switch ((prefixBytes[2] >> 2) & 0x01)
                        {
                        case 0:
                            // Decode MVEX-prefix
                            instruction->encoding = ZYDIS_INSTRUCTION_ENCODING_MVEX;
                            ZYDIS_CHECK(ZydisDecodeMVEX(context, instruction, prefixBytes));
                            instruction->opcodeMap = 
                                ZYDIS_OPCODE_MAP_EX0 + instruction->details.mvex.mmmm;
                            break;
                        case 1:
                            // Decode EVEX-prefix
                            instruction->encoding = ZYDIS_INSTRUCTION_ENCODING_EVEX;
                            ZYDIS_CHECK(ZydisDecodeEVEX(context, instruction, prefixBytes));
                            instruction->opcodeMap = 
                                ZYDIS_OPCODE_MAP_EX0 + instruction->details.evex.mm;
                            break;
                        default:
                            ZYDIS_UNREACHABLE;
                        }
                        break;
                    default:
                        ZYDIS_UNREACHABLE;
                    }
                }
                break;
            } 
            case 0x8F:
            {
                uint8_t nextInput;
                ZYDIS_CHECK(ZydisInputPeek(context, instruction, &nextInput));
                if ((nextInput & 0x1F) >= 8)
                {
                    if (instruction->attributes & ZYDIS_ATTRIB_HAS_REX)
                    {
                        return ZYDIS_STATUS_ILLEGAL_REX;
                    }
                    if (context->mandatoryCandidate)
                    {
                        return ZYDIS_STATUS_ILLEGAL_LEGACY_PFX;
                    }
                    uint8_t prefixBytes[3] = { 0x8F, 0x00, 0x00 };
                    // Read additional xop-prefix data
                    ZYDIS_ASSERT(!instruction->details.xop.isDecoded);
                    ZYDIS_CHECK(ZydisInputNext(context, instruction, &prefixBytes[1]));
                    ZYDIS_CHECK(ZydisInputNext(context, instruction, &prefixBytes[2]));
                    //ZYDIS_CHECK(ZydisInputNextBytes(context, instruction, &prefixBytes[1], 2));
                    // Decode xop-prefix
                    instruction->encoding = ZYDIS_INSTRUCTION_ENCODING_XOP;
                    ZYDIS_CHECK(ZydisDecodeXOP(context, instruction, prefixBytes));
                    instruction->opcodeMap = 
                        ZYDIS_OPCODE_MAP_XOP8 + instruction->details.xop.m_mmmm - 0x08;
                }
                break;
            }
            default:
                break;
            }
            break;
        case ZYDIS_OPCODE_MAP_0F:
            switch (instruction->opcode)
            {
            case 0x0F:
                instruction->encoding = ZYDIS_INSTRUCTION_ENCODING_3DNOW;
                instruction->opcodeMap = ZYDIS_OPCODE_MAP_DEFAULT;
                break;
            case 0x38:
                instruction->opcodeMap = ZYDIS_OPCODE_MAP_0F38;
                break;
            case 0x3A:
                instruction->opcodeMap = ZYDIS_OPCODE_MAP_0F3A;
                break;
            default:
                break;
            }
            break;
        case ZYDIS_OPCODE_MAP_0F38:
        case ZYDIS_OPCODE_MAP_0F3A:
        case ZYDIS_OPCODE_MAP_XOP8:
        case ZYDIS_OPCODE_MAP_XOP9:
        case ZYDIS_OPCODE_MAP_XOPA:
            // Nothing to do here
            break;
        default:
            ZYDIS_UNREACHABLE;
        }
        break;
    case ZYDIS_INSTRUCTION_ENCODING_3DNOW:
        // All 3DNOW (0x0F 0x0F) instructions are using the same operand encoding. We just 
        // decode a random (pi2fw) instruction and extract the actual opcode later.
        *index = 0x0C;
        return ZYDIS_STATUS_SUCCESS;
    default:
        ZYDIS_CHECK(ZydisInputNext(context, instruction, &instruction->opcode));
        break;
    }

    *index = instruction->opcode; 
    return ZYDIS_STATUS_SUCCESS;
}

static ZydisStatus ZydisNodeHandlerMode(ZydisDecoderContext* context, uint16_t* index)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(index);

    switch (context->decoder->machineMode)
    {
    case 16:
        *index = 0;
        break;
    case 32:
        *index = 1;
        break;
    case 64:
        *index = 2;
        break;
    default:
        ZYDIS_UNREACHABLE;
    }
    return ZYDIS_STATUS_SUCCESS;   
}

static ZydisStatus ZydisNodeHandlerModeCompact(ZydisDecoderContext* context, uint16_t* index)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(index);

    *index = (context->decoder->machineMode == ZYDIS_MACHINE_MODE_LONG_64) ? 0 : 1;
    return ZYDIS_STATUS_SUCCESS;   
}

static ZydisStatus ZydisNodeHandlerModrmMod(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint16_t* index)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(index);

    if (!instruction->details.modrm.isDecoded)
    {
        uint8_t modrmByte;
        ZYDIS_CHECK(ZydisInputNext(context, instruction, &modrmByte));
        ZydisDecodeModRM(instruction, modrmByte);               
    }
    *index = instruction->details.modrm.mod;
    return ZYDIS_STATUS_SUCCESS;
}

static ZydisStatus ZydisNodeHandlerModrmModCompact(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint16_t* index)
{
    ZYDIS_CHECK(ZydisNodeHandlerModrmMod(context, instruction, index));
    *index = (*index == 0x3) ? 0 : 1;
    return ZYDIS_STATUS_SUCCESS;
}

static ZydisStatus ZydisNodeHandlerModrmReg(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint16_t* index)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(index);

    if (!instruction->details.modrm.isDecoded)
    {
        uint8_t modrmByte;
        ZYDIS_CHECK(ZydisInputNext(context, instruction, &modrmByte));
        ZydisDecodeModRM(instruction, modrmByte);               
    }
    *index = instruction->details.modrm.reg;
    return ZYDIS_STATUS_SUCCESS;
}

static ZydisStatus ZydisNodeHandlerModrmRm(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint16_t* index)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(index);

    if (!instruction->details.modrm.isDecoded)
    {
        uint8_t modrmByte;
        ZYDIS_CHECK(ZydisInputNext(context, instruction, &modrmByte));
        ZydisDecodeModRM(instruction, modrmByte);                
    }
    *index = instruction->details.modrm.rm;
    return ZYDIS_STATUS_SUCCESS;
}

static ZydisStatus ZydisNodeHandlerMandatoryPrefix(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint16_t* index)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(index);

    switch (context->mandatoryCandidate)
    {
    case 0x66:
        instruction->attributes &= ~ZYDIS_ATTRIB_HAS_OPERANDSIZE;
        *index = 2;
        break;
    case 0xF3:
        *index = 3;
        break;
    case 0xF2:
        *index = 4;
        break;
    default:
        *index = 1;
        break;
    }
    // TODO: Consume prefix and make sure it's available again, if we need to fallback

    return ZYDIS_STATUS_SUCCESS;
}

static ZydisStatus ZydisNodeHandlerOperandSize(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint16_t* index)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(index);

    if ((context->decoder->machineMode == 64) && (context->cache.W))
    {
        *index = 2;
    } else
    {
        switch (context->decoder->machineMode)
        {
        case 16:
            *index = (instruction->attributes & ZYDIS_ATTRIB_HAS_OPERANDSIZE) ? 1 : 0;
            break;
        case 32:
        case 64:
            *index = (instruction->attributes & ZYDIS_ATTRIB_HAS_OPERANDSIZE) ? 0 : 1;
            break;
        default:
            ZYDIS_UNREACHABLE;
        }
    }   

    return ZYDIS_STATUS_SUCCESS;   
}

static ZydisStatus ZydisNodeHandlerAddressSize(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint16_t* index)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(index);

    switch (context->decoder->addressWidth)
    {
    case 16:
        *index = (instruction->attributes & ZYDIS_ATTRIB_HAS_ADDRESSSIZE) ? 1 : 0;
        break;
    case 32:
        *index = (instruction->attributes & ZYDIS_ATTRIB_HAS_ADDRESSSIZE) ? 0 : 1;
        break;
    case 64:
        *index = (instruction->attributes & ZYDIS_ATTRIB_HAS_ADDRESSSIZE) ? 1 : 2;
        break;
    default: 
        ZYDIS_UNREACHABLE;
    }
    return ZYDIS_STATUS_SUCCESS;   
}

static ZydisStatus ZydisNodeHandlerVectorLength(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint16_t* index)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(index);

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
        ZYDIS_ASSERT(instruction->details.xop.isDecoded);
        break;
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
        ZYDIS_ASSERT(instruction->details.vex.isDecoded);
        break;
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
        ZYDIS_ASSERT(instruction->details.evex.isDecoded);
        break;
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
        ZYDIS_ASSERT(instruction->details.mvex.isDecoded);
        break;
    default:
        ZYDIS_UNREACHABLE;
    }
    *index = context->cache.LL;
    if (*index == 3)
    {
        return ZYDIS_STATUS_DECODING_ERROR;
    }
    return ZYDIS_STATUS_SUCCESS;
}

static ZydisStatus ZydisNodeHandlerRexW(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint16_t* index)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(index);

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_DEFAULT:
        // nothing to do here       
        break;
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
        ZYDIS_ASSERT(instruction->details.xop.isDecoded);
        break;
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
        ZYDIS_ASSERT(instruction->details.vex.isDecoded);
        break;
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
        ZYDIS_ASSERT(instruction->details.evex.isDecoded);
        break;
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
        ZYDIS_ASSERT(instruction->details.mvex.isDecoded);
        break;
    default:
        ZYDIS_UNREACHABLE;
    }
    *index = context->cache.W;
    return ZYDIS_STATUS_SUCCESS;
}

static ZydisStatus ZydisNodeHandlerRexB(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction, uint16_t* index)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(index);

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_DEFAULT:
        // nothing to do here       
        break;
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
        ZYDIS_ASSERT(instruction->details.xop.isDecoded);
        break;
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
        ZYDIS_ASSERT(instruction->details.vex.isDecoded);
        break;
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
        ZYDIS_ASSERT(instruction->details.evex.isDecoded);
        break;
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
        ZYDIS_ASSERT(instruction->details.mvex.isDecoded);
        break;
    default:
        ZYDIS_UNREACHABLE;
    }
    *index = context->cache.B;
    return ZYDIS_STATUS_SUCCESS;
}

static ZydisStatus ZydisNodeHandlerEvexB(ZydisDecodedInstruction* instruction, uint16_t* index)
{
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(index);

    ZYDIS_ASSERT(instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX);
    ZYDIS_ASSERT(instruction->details.evex.isDecoded);
    *index = instruction->details.evex.b;
    return ZYDIS_STATUS_SUCCESS;   
}

static ZydisStatus ZydisNodeHandlerEvexZ(ZydisDecodedInstruction* instruction, uint16_t* index)
{
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(index);

    ZYDIS_ASSERT(instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX);
    ZYDIS_ASSERT(instruction->details.evex.isDecoded);
    *index = instruction->details.evex.z;
    return ZYDIS_STATUS_SUCCESS;   
}

static ZydisStatus ZydisNodeHandlerMvexE(ZydisDecodedInstruction* instruction, uint16_t* index)
{
    ZYDIS_ASSERT(instruction);
    ZYDIS_ASSERT(index);

    ZYDIS_ASSERT(instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_MVEX);
    ZYDIS_ASSERT(instruction->details.mvex.isDecoded);
    *index = instruction->details.mvex.E;
    return ZYDIS_STATUS_SUCCESS;   
}

/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Uses the instruction-tree to decode the current instruction.
 *
 * @param   context     A pointer to the @c ZydisDecoderContext instance.
 * @param   instruction A pointer to the @c ZydisDecodedInstruction struct.
 *
 * @return  A zydis decoder status code.
 */
static ZydisStatus ZydisDecodeInstruction(ZydisDecoderContext* context, 
    ZydisDecodedInstruction* instruction)
{
    ZYDIS_ASSERT(context);
    ZYDIS_ASSERT(instruction);

    // Iterate through the instruction table
    const ZydisInstructionTreeNode* node = ZydisInstructionTreeGetRootNode();
    const ZydisInstructionTreeNode* temp = NULL;
    ZydisInstructionTreeNodeType nodeType;
    do
    {
        nodeType = node->type;
        uint16_t index = 0;
        ZydisStatus status = 0;
        switch (nodeType)
        {
        case ZYDIS_NODETYPE_INVALID:
            if (temp)
            {
                node = temp;
                temp = NULL;
                nodeType = ZYDIS_NODETYPE_FILTER_MANDATORY_PREFIX;
                if (context->mandatoryCandidate == 0x66)
                {
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_OPERANDSIZE;
                }
                continue;
            }
            return ZYDIS_STATUS_DECODING_ERROR;
        case ZYDIS_NODETYPE_FILTER_XOP:
            status = ZydisNodeHandlerXOP(instruction, &index);
            break; 
        case ZYDIS_NODETYPE_FILTER_VEX:
            status = ZydisNodeHandlerVEX(instruction, &index);
            break;  
        case ZYDIS_NODETYPE_FILTER_EMVEX:
            status = ZydisNodeHandlerEMVEX(instruction, &index);
            break; 
        case ZYDIS_NODETYPE_FILTER_OPCODE:
            status = ZydisNodeHandlerOpcode(context, instruction, &index);
            break;            
        case ZYDIS_NODETYPE_FILTER_MODE:
            status = ZydisNodeHandlerMode(context, &index);
            break; 
        case ZYDIS_NODETYPE_FILTER_MODE_COMPACT:
            status = ZydisNodeHandlerModeCompact(context, &index);
            break; 
        case ZYDIS_NODETYPE_FILTER_MODRM_MOD:
            status = ZydisNodeHandlerModrmMod(context, instruction, &index);
            break; 
        case ZYDIS_NODETYPE_FILTER_MODRM_MOD_COMPACT:
            status = ZydisNodeHandlerModrmModCompact(context, instruction, &index);
            break; 
        case ZYDIS_NODETYPE_FILTER_MODRM_REG:
            status = ZydisNodeHandlerModrmReg(context, instruction, &index);
            break;       
        case ZYDIS_NODETYPE_FILTER_MODRM_RM:
            status = ZydisNodeHandlerModrmRm(context, instruction, &index);
            break; 
        case ZYDIS_NODETYPE_FILTER_MANDATORY_PREFIX:
            status = ZydisNodeHandlerMandatoryPrefix(context, instruction, &index);
            temp = ZydisInstructionTreeGetChildNode(node, 0);
            // TODO: Return to this point, if index == 0 contains a value and the previous path
            // TODO: was not successfull
            // TODO: Restore consumed prefix
            break; 
        case ZYDIS_NODETYPE_FILTER_OPERAND_SIZE:
            status = ZydisNodeHandlerOperandSize(context, instruction, &index);
            break;    
        case ZYDIS_NODETYPE_FILTER_ADDRESS_SIZE:
            status = ZydisNodeHandlerAddressSize(context, instruction, &index);
            break; 
        case ZYDIS_NODETYPE_FILTER_VECTOR_LENGTH:
            status = ZydisNodeHandlerVectorLength(context, instruction, &index);
            break; 
        case ZYDIS_NODETYPE_FILTER_REX_W:
            status = ZydisNodeHandlerRexW(context, instruction, &index);
            break; 
        case ZYDIS_NODETYPE_FILTER_REX_B:
            status = ZydisNodeHandlerRexB(context, instruction, &index);
            break;
        case ZYDIS_NODETYPE_FILTER_EVEX_B:
            status = ZydisNodeHandlerEvexB(instruction, &index);
            break;  
        case ZYDIS_NODETYPE_FILTER_EVEX_Z:
            status = ZydisNodeHandlerEvexZ(instruction, &index);
            break; 
        case ZYDIS_NODETYPE_FILTER_MVEX_E:
            status = ZydisNodeHandlerMvexE(instruction, &index);
            break;                           
        default:
            if (nodeType & ZYDIS_NODETYPE_DEFINITION_MASK)
            { 
                const ZydisInstructionDefinition* definition;
                ZydisGetInstructionDefinition(node, &definition);
                ZydisSetEffectiveOperandSize(context, instruction, definition);
                ZydisSetEffectiveAddressWidth(context, instruction);

                const ZydisInstructionParts* optionalParts;
                ZydisGetOptionalInstructionParts(node, &optionalParts);
                ZYDIS_CHECK(ZydisDecodeOptionalInstructionParts(context, instruction, optionalParts));

                ZydisBool hasNDSNDDOperand = ZYDIS_FALSE;
                ZydisBool hasVSIB = ZYDIS_FALSE;
                ZydisMaskPolicy maskPolicy = ZYDIS_MASK_POLICY_INVALID;
                switch (instruction->encoding)
                {
                case ZYDIS_INSTRUCTION_ENCODING_DEFAULT:
                    break;
                case ZYDIS_INSTRUCTION_ENCODING_3DNOW:
                {
                    // Get actual 3dnow opcode and definition
                    ZYDIS_CHECK(ZydisInputNext(context, instruction, &instruction->opcode));
                    node = ZydisInstructionTreeGetRootNode();
                    node = ZydisInstructionTreeGetChildNode(node, 0x0F);
                    node = ZydisInstructionTreeGetChildNode(node, 0x0F);
                    node = ZydisInstructionTreeGetChildNode(node, instruction->opcode);
                    if (node->type == ZYDIS_NODETYPE_INVALID)
                    {
                        return ZYDIS_STATUS_DECODING_ERROR;        
                    }
                    ZYDIS_ASSERT(node->type == ZYDIS_NODETYPE_FILTER_MODRM_MOD_COMPACT);
                    node = ZydisInstructionTreeGetChildNode(
                        node, (instruction->details.modrm.mod == 0x3) ? 0 : 1);
                    ZydisGetInstructionDefinition(node, &definition);
                    break;
                }
                case ZYDIS_INSTRUCTION_ENCODING_XOP:
                {
                    const ZydisInstructionDefinitionXOP* def = 
                        (const ZydisInstructionDefinitionXOP*)definition;
                    hasNDSNDDOperand = def->hasNDSNDDOperand;
                    break;
                }
                case ZYDIS_INSTRUCTION_ENCODING_VEX:
                {
                    const ZydisInstructionDefinitionVEX* def = 
                        (const ZydisInstructionDefinitionVEX*)definition;
                    hasNDSNDDOperand = def->hasNDSNDDOperand;
                    break;
                }
                case ZYDIS_INSTRUCTION_ENCODING_EVEX:
                {
                    const ZydisInstructionDefinitionEVEX* def = 
                        (const ZydisInstructionDefinitionEVEX*)definition;
                    hasNDSNDDOperand = def->hasNDSNDDOperand;
                    hasVSIB = def->hasVSIB;
                    maskPolicy = def->maskPolicy;
                    break;
                }
                case ZYDIS_INSTRUCTION_ENCODING_MVEX:
                {
                    const ZydisInstructionDefinitionMVEX* def = 
                        (const ZydisInstructionDefinitionMVEX*)definition;
                    hasNDSNDDOperand = def->hasNDSNDDOperand;
                    hasVSIB = def->hasVSIB;
                    maskPolicy = def->maskPolicy;

                    // Check for invalid MVEX.SSS values
                    static const uint8_t lookup[26][8] =
                    {
                        // ZYDIS_MVEX_FUNC_IGNORED
                        { 1, 1, 1, 1, 1, 1, 1, 1 },
                        // ZYDIS_MVEX_FUNC_INVALID
                        { 1, 0, 0, 0, 0, 0, 0, 0 },
                        // ZYDIS_MVEX_FUNC_RC
                        { 1, 1, 1, 1, 1, 1, 1, 1 },
                        // ZYDIS_MVEX_FUNC_SAE
                        { 1, 1, 1, 1, 1, 1, 1, 1 },
                        // ZYDIS_MVEX_FUNC_F_32
                        { 1, 0, 0, 0, 0, 0, 0, 0 },
                        // ZYDIS_MVEX_FUNC_I_32
                        { 1, 0, 0, 0, 0, 0, 0, 0 },
                        // ZYDIS_MVEX_FUNC_F_64
                        { 1, 0, 0, 0, 0, 0, 0, 0 },
                        // ZYDIS_MVEX_FUNC_I_64
                        { 1, 0, 0, 0, 0, 0, 0, 0 },
                        // ZYDIS_MVEX_FUNC_SWIZZLE_32
                        { 1, 1, 1, 1, 1, 1, 1, 1 },
                        // ZYDIS_MVEX_FUNC_SWIZZLE_64
                        { 1, 1, 1, 1, 1, 1, 1, 1 },
                        // ZYDIS_MVEX_FUNC_SF_32
                        { 1, 1, 1, 1, 1, 1, 1, 1 },
                        // ZYDIS_MVEX_FUNC_SF_32_BCST
                        { 1, 1, 1, 0, 0, 0, 0, 0 },
                        // ZYDIS_MVEX_FUNC_SF_32_BCST_4TO16
                        { 1, 0, 1, 0, 0, 0, 0, 0 },
                        // ZYDIS_MVEX_FUNC_SF_64
                        { 1, 1, 1, 0, 0, 0, 0, 0 },
                        // ZYDIS_MVEX_FUNC_SI_32
                        { 1, 1, 1, 0, 1, 1, 1, 1 },
                        // ZYDIS_MVEX_FUNC_SI_32_BCST
                        { 1, 1, 1, 0, 0, 0, 0, 0 },
                        // ZYDIS_MVEX_FUNC_SI_32_BCST_4TO16
                        { 1, 0, 1, 0, 0, 0, 0, 0 },
                        // ZYDIS_MVEX_FUNC_SI_64
                        { 1, 1, 1, 0, 0, 0, 0, 0 },
                        // ZYDIS_MVEX_FUNC_UF_32
                        { 1, 0, 0, 1, 1, 1, 1, 1 },
                        // ZYDIS_MVEX_FUNC_UF_64
                        { 1, 0, 0, 0, 0, 0, 0, 0 },
                        // ZYDIS_MVEX_FUNC_UI_32
                        { 1, 0, 0, 0, 1, 1, 1, 1 },
                        // ZYDIS_MVEX_FUNC_UI_64
                        { 1, 0, 0, 0, 0, 0, 0, 0 },
                        // ZYDIS_MVEX_FUNC_DF_32
                        { 1, 0, 0, 1, 1, 1, 1, 1 },
                        // ZYDIS_MVEX_FUNC_DF_64
                        { 1, 0, 0, 0, 0, 0, 0, 0 },
                        // ZYDIS_MVEX_FUNC_DI_32
                        { 1, 0, 0, 0, 1, 1, 1, 1 },
                        // ZYDIS_MVEX_FUNC_DI_64
                        { 1, 0, 0, 0, 0, 0, 0, 0 }
                    };
                    ZYDIS_ASSERT(def->functionality < ZYDIS_ARRAY_SIZE(lookup));
                    ZYDIS_ASSERT(instruction->details.mvex.SSS < 8);
                    if (!lookup[def->functionality][instruction->details.mvex.SSS])
                    {
                        return ZYDIS_STATUS_DECODING_ERROR;
                    }
                    break;
                }
                default:
                    ZYDIS_UNREACHABLE;
                }

                // Check for invalid `XOP/VEX/EVEX/MVEX.vvvv` value
                if (!hasNDSNDDOperand && (context->cache.v_vvvv & 0x0F))
                {
                    return ZYDIS_STATUS_DECODING_ERROR;
                }

                // Check for invalid `EVEX/MVEX.v'` value
                if (!hasNDSNDDOperand && !hasVSIB && context->cache.V2)
                {
                    return ZYDIS_STATUS_DECODING_ERROR;
                }

                // Check for invalid MASK registers
                switch (maskPolicy)
                {
                case ZYDIS_MASK_POLICY_INVALID:
                case ZYDIS_MASK_POLICY_ALLOWED:
                    // Nothing to do here
                    break;
                case ZYDIS_MASK_POLICY_REQUIRED:
                    if (!context->cache.mask)
                    {
                        return ZYDIS_STATUS_INVALID_MASK;
                    }
                    break;
                case ZYDIS_MASK_POLICY_FORBIDDEN:
                    if (context->cache.mask)
                    {
                        return ZYDIS_STATUS_INVALID_MASK;
                    }
                    break;
                default:
                    ZYDIS_UNREACHABLE;
                }

                instruction->mnemonic = definition->mnemonic;

                if (context->decoder->decodeGranularity == ZYDIS_DECODE_GRANULARITY_FULL)
                {
                    ZydisSetPrefixRelatedAttributes(context, instruction, definition);
                    switch (instruction->encoding)
                    {
                    case ZYDIS_INSTRUCTION_ENCODING_XOP:
                    case ZYDIS_INSTRUCTION_ENCODING_VEX:
                    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
                    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
                        ZydisSetAVXInformation(context, instruction, definition);
                        break;
                    default:
                        break;
                    }
                    ZYDIS_CHECK(ZydisDecodeOperands(context, instruction, definition));
                }

                return ZYDIS_STATUS_SUCCESS;
            }
            ZYDIS_UNREACHABLE;
        }
        ZYDIS_CHECK(status);
        node = ZydisInstructionTreeGetChildNode(node, index);
    } while((nodeType != ZYDIS_NODETYPE_INVALID) && !(nodeType & ZYDIS_NODETYPE_DEFINITION_MASK));
    return ZYDIS_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

ZydisStatus ZydisDecoderInitInstructionDecoder(ZydisInstructionDecoder* decoder, 
    ZydisMachineMode machineMode, ZydisAddressWidth addressWidth)
{
    return ZydisDecoderInitInstructionDecoderEx(
        decoder, machineMode, addressWidth, ZYDIS_DECODE_GRANULARITY_DEFAULT);
}

ZydisStatus ZydisDecoderInitInstructionDecoderEx(ZydisInstructionDecoder* decoder, 
    ZydisMachineMode machineMode, ZydisAddressWidth addressWidth, 
    ZydisDecodeGranularity decodeGranularity)
{
    if (!decoder || ((machineMode != 16) && (machineMode != 32) && (machineMode != 64)) ||
        ((decodeGranularity != ZYDIS_DECODE_GRANULARITY_DEFAULT) && 
         (decodeGranularity != ZYDIS_DECODE_GRANULARITY_FULL) && 
         (decodeGranularity != ZYDIS_DECODE_GRANULARITY_MINIMAL)))
    {
        return ZYDIS_STATUS_INVALID_PARAMETER;
    }
    if (machineMode == 64)
    {
        addressWidth = ZYDIS_ADDRESS_WIDTH_64;
    } else
    {
        if ((addressWidth != 16) && (addressWidth != 32))
        {
            return ZYDIS_STATUS_INVALID_PARAMETER;
        }
    }
    if (decodeGranularity == ZYDIS_DECODE_GRANULARITY_DEFAULT)
    {
        decodeGranularity = ZYDIS_DECODE_GRANULARITY_FULL;
    }

    decoder->machineMode = machineMode;
    decoder->addressWidth = addressWidth;
    decoder->decodeGranularity = decodeGranularity;

    return ZYDIS_STATUS_SUCCESS;
}

ZydisStatus ZydisDecoderDecodeBuffer(const ZydisInstructionDecoder* decoder, const void* buffer, 
    size_t bufferLen, uint64_t instructionPointer, ZydisDecodedInstruction* instruction)
{
    if (!decoder)
    {
        return ZYDIS_STATUS_INVALID_PARAMETER;
    }

    if (!buffer || !bufferLen)
    {
        return ZYDIS_STATUS_NO_MORE_DATA;
    }

    ZydisDecoderContext context;
    memset(&context.cache, 0, sizeof(context.cache));
    context.decoder = decoder;
    context.buffer = (uint8_t*)buffer;
    context.bufferLen = bufferLen;
    context.lastSegmentPrefix = 0;
    context.mandatoryCandidate = 0;

    void* userData = instruction->userData;
    memset(instruction, 0, sizeof(*instruction));   
    instruction->machineMode = decoder->machineMode;
    instruction->instrAddress = instructionPointer;
    instruction->userData = userData;

    ZYDIS_CHECK(ZydisCollectOptionalPrefixes(&context, instruction));
    ZYDIS_CHECK(ZydisDecodeInstruction(&context, instruction));

    instruction->instrPointer = instruction->instrAddress + instruction->length;

    // TODO: The index, dest and mask regs for AVX2 gathers must be different.

    // TODO: More EVEX UD conditions (page 81)

    // TODO: Set AVX-512 info

    return ZYDIS_STATUS_SUCCESS;
}

/* ============================================================================================== */
