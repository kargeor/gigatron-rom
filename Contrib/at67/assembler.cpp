#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <fstream>
#include <sstream>
#include <iterator>
#include <algorithm>
#include <cstdarg>

#ifndef STAND_ALONE
#include "cpu.h"
#include "editor.h"
#endif

#include "audio.h"
#include "loader.h"
#include "assembler.h"
#include "expression.h"


#define BRANCH_ADJUSTMENT    2


namespace Assembler
{
    enum ParseType {PreProcessPass=0, MnemonicPass, CodePass, NumParseTypes};
    enum ByteSize {BadSize=-1, OneByte=1, TwoBytes=2, ThreeBytes=3};
    enum EvaluateResult {Failed=-1, NotFound, Reserved, Duplicate, Skipped, Success};
    enum OpcodeType {ReservedDB=0, ReservedDW, ReservedDBR, ReservedDWR, vCpu, Native};
    enum AddressMode {D_AC=0b00000000, X_AC=0b00000100, YD_AC=0b00001000, YX_AC=0b00001100, D_X=0b00010000, D_Y=0b00010100, D_OUT=0b00011000, YXpp_OUT=0b00011100};
    enum BusMode {D=0b00000000, RAM=0b00000001, AC=0b00000010, IN=0b00000011};
    enum ReservedWords {CallTable=0, StartAddress, SingleStepWatch, DisableUpload, CpuUsageAddressA, CpuUsageAddressB, INCLUDE, MACRO, ENDM, GPRINTF, NumReservedWords};


    struct Label
    {
        uint16_t _address;
        std::string _name;
    };

    struct Equate
    {
        bool _isCustomAddress;
        uint16_t _operand;
        std::string _name;
    };

    struct Instruction
    {
        bool _isRomAddress;
        bool _isCustomAddress;
        ByteSize _byteSize;
        uint8_t _opcode;
        uint8_t _operand0;
        uint8_t _operand1;
        uint16_t _address;
        OpcodeType _opcodeType;
    };

    struct InstructionType
    {
        uint8_t _opcode;
        uint8_t _branch;
        ByteSize _byteSize;
        OpcodeType _opcodeType;
    };

    struct CallTableEntry
    {
        uint8_t _operand;
        uint16_t _address;
    };

    struct Macro
    {
        bool _complete = false;
        bool _fromInclude = false;
        int _startLine = 0;
        int _endLine = 0;
        int _fileStartLine;
        std::string _name;
        std::string _filename;
        std::vector<std::string> _params;
        std::vector<std::string> _lines;
    };

    struct LineToken
    {
        bool _fromInclude = false;
        int _includeLineNumber;
        std::string _text;
        std::string _includeName;
    };

    struct Gprintf
    {
        enum Type {Chr, Int, Bin, Oct, Hex, Str};
        struct Var
        {
            bool _indirect = false;
            Type _type;
            int _width;
            uint16_t _data;
            std::string _var;
        };

        bool _displayed = false;
        uint16_t _address;
        int _lineNumber;
        std::string _lineToken;
        std::string _format;
        std::vector<Var> _vars;
        std::vector<std::string> _subs;
    };


    int _lineNumber;

    uint16_t _byteCount = 0;
    uint16_t _callTable = DEFAULT_CALL_TABLE;
    uint16_t _startAddress = DEFAULT_START_ADDRESS;
    uint16_t _currentAddress = _startAddress;

    std::string _includePath = "";

    std::vector<Label> _labels;
    std::vector<Equate> _equates;
    std::vector<Instruction> _instructions;
    std::vector<ByteCode> _byteCode;
    std::vector<CallTableEntry> _callTableEntries;
    std::vector<std::string> _reservedWords;
    std::vector<Gprintf> _gprintfs;

    uint16_t getStartAddress(void) {return _startAddress;}
    void setIncludePath(const std::string& includePath) {_includePath = includePath;}


    void initialise(void)
    {
        _reservedWords.push_back("_callTable_");
        _reservedWords.push_back("_startAddress_");
        _reservedWords.push_back("_singleStepWatch_");
        _reservedWords.push_back("_disableUpload_");
        _reservedWords.push_back("_cpuUsageAddressA_");
        _reservedWords.push_back("_cpuUsageAddressB_");
        _reservedWords.push_back("%include");
        _reservedWords.push_back("%MACRO");
        _reservedWords.push_back("%ENDM");
        _reservedWords.push_back("gprintf");
    }

    // Returns true when finished
    bool getNextAssembledByte(ByteCode& byteCode, bool debug)
    {
        static bool isUserCode = false;

        if(_byteCount >= _byteCode.size())
        {
            _byteCount = 0;
            if(debug  &&  isUserCode) fprintf(stderr, "\n");
            return true;
        }

        static uint16_t address = 0x0000;
        static uint16_t customAddress = 0x0000;

        // Get next byte
        if(_byteCount == 0) address = _startAddress;
        byteCode = _byteCode[_byteCount++];

        // New section
        if(byteCode._isCustomAddress)
        {
            address = byteCode._address;
            customAddress = byteCode._address;
        }

        // User code is RAM code or ROM code in user ROM space
        isUserCode = !byteCode._isRomAddress  ||  (byteCode._isRomAddress  &&  customAddress >= USER_ROM_ADDRESS);

        // Seperate sections
        if(debug  &&  byteCode._isCustomAddress  &&  isUserCode) fprintf(stderr, "\n");

        // 16bit for ROM, 8bit for RAM
        if(debug  &&  isUserCode)
        {
            if(byteCode._isRomAddress)
            {
                if((address & 0x0001) == 0x0000)
                {
                    fprintf(stderr, "Assembler::getNextAssembledByte() : ROM : %04X  %02X", customAddress + ((address & 0x00FF)>>1), byteCode._data);
                }
                else
                {
                    fprintf(stderr, "%02X\n", byteCode._data);
                }
            }
            else
            {
                fprintf(stderr, "Assembler::getNextAssembledByte() : RAM : %04X  %02X\n", address, byteCode._data);
            }
        }
        address++;
        return false;
    }    

    InstructionType getOpcode(const std::string& input)
    {
        InstructionType instructionType = {0x00, 0x00, BadSize, vCpu};

        std::string token = input;
        Expression::strToUpper(token);

        // Gigatron vCPU instructions
        if(token == "ST")         {instructionType._opcode = 0x5E; instructionType._byteSize = TwoBytes;  }
        else if(token == "STW")   {instructionType._opcode = 0x2B; instructionType._byteSize = TwoBytes;  }
        else if(token == "STLW")  {instructionType._opcode = 0xEC; instructionType._byteSize = TwoBytes;  }
        else if(token == "LD")    {instructionType._opcode = 0x1A; instructionType._byteSize = TwoBytes;  }
        else if(token == "LDI")   {instructionType._opcode = 0x59; instructionType._byteSize = TwoBytes;  }
        else if(token == "LDWI")  {instructionType._opcode = 0x11; instructionType._byteSize = ThreeBytes;}
        else if(token == "LDW")   {instructionType._opcode = 0x21; instructionType._byteSize = TwoBytes;  }
        else if(token == "LDLW")  {instructionType._opcode = 0xEE; instructionType._byteSize = TwoBytes;  }
        else if(token == "ADDW")  {instructionType._opcode = 0x99; instructionType._byteSize = TwoBytes;  }
        else if(token == "SUBW")  {instructionType._opcode = 0xB8; instructionType._byteSize = TwoBytes;  }
        else if(token == "ADDI")  {instructionType._opcode = 0xE3; instructionType._byteSize = TwoBytes;  }
        else if(token == "SUBI")  {instructionType._opcode = 0xE6; instructionType._byteSize = TwoBytes;  }
        else if(token == "LSLW")  {instructionType._opcode = 0xE9; instructionType._byteSize = OneByte;   }
        else if(token == "INC")   {instructionType._opcode = 0x93; instructionType._byteSize = TwoBytes;  }
        else if(token == "ANDI")  {instructionType._opcode = 0x82; instructionType._byteSize = TwoBytes;  }
        else if(token == "ANDW")  {instructionType._opcode = 0xF8; instructionType._byteSize = TwoBytes;  }
        else if(token == "ORI")   {instructionType._opcode = 0x88; instructionType._byteSize = TwoBytes;  }
        else if(token == "ORW")   {instructionType._opcode = 0xFA; instructionType._byteSize = TwoBytes;  }
        else if(token == "XORI")  {instructionType._opcode = 0x8C; instructionType._byteSize = TwoBytes;  }
        else if(token == "XORW")  {instructionType._opcode = 0xFC; instructionType._byteSize = TwoBytes;  }
        else if(token == "PEEK")  {instructionType._opcode = 0xAD; instructionType._byteSize = OneByte;   }
        else if(token == "DEEK")  {instructionType._opcode = 0xF6; instructionType._byteSize = OneByte;   }
        else if(token == "POKE")  {instructionType._opcode = 0xF0; instructionType._byteSize = TwoBytes;  }
        else if(token == "DOKE")  {instructionType._opcode = 0xF3; instructionType._byteSize = TwoBytes;  }
        else if(token == "LUP")   {instructionType._opcode = 0x7F; instructionType._byteSize = TwoBytes;  }
        else if(token == "BRA")   {instructionType._opcode = 0x90; instructionType._byteSize = TwoBytes;  }
        else if(token == "CALL")  {instructionType._opcode = 0xCF; instructionType._byteSize = TwoBytes;  }
        else if(token == "RET")   {instructionType._opcode = 0xFF; instructionType._byteSize = OneByte;   }
        else if(token == "PUSH")  {instructionType._opcode = 0x75; instructionType._byteSize = OneByte;   }
        else if(token == "POP")   {instructionType._opcode = 0x63; instructionType._byteSize = OneByte;   }
        else if(token == "ALLOC") {instructionType._opcode = 0xDF; instructionType._byteSize = TwoBytes;  }
        else if(token == "SYS")   {instructionType._opcode = 0xB4; instructionType._byteSize = TwoBytes;  }
        else if(token == "DEF")   {instructionType._opcode = 0xCD; instructionType._byteSize = TwoBytes;  }

        // Gigatron vCPU branch instructions
        else if(token == "BEQ")   {instructionType._opcode = 0x35; instructionType._branch = 0x3F; instructionType._byteSize = ThreeBytes;}
        else if(token == "BNE")   {instructionType._opcode = 0x35; instructionType._branch = 0x72; instructionType._byteSize = ThreeBytes;}
        else if(token == "BLT")   {instructionType._opcode = 0x35; instructionType._branch = 0x50; instructionType._byteSize = ThreeBytes;}
        else if(token == "BGT")   {instructionType._opcode = 0x35; instructionType._branch = 0x4D; instructionType._byteSize = ThreeBytes;}
        else if(token == "BLE")   {instructionType._opcode = 0x35; instructionType._branch = 0x56; instructionType._byteSize = ThreeBytes;}
        else if(token == "BGE")   {instructionType._opcode = 0x35; instructionType._branch = 0x53; instructionType._byteSize = ThreeBytes;}

        // Reserved assembler opcodes
        else if(token == "DB")    {instructionType._byteSize = TwoBytes;   instructionType._opcodeType = ReservedDB; }
        else if(token == "DW")    {instructionType._byteSize = ThreeBytes; instructionType._opcodeType = ReservedDW; }
        else if(token == "DBR")   {instructionType._byteSize = TwoBytes;   instructionType._opcodeType = ReservedDBR;}
        else if(token == "DWR")   {instructionType._byteSize = ThreeBytes; instructionType._opcodeType = ReservedDWR;}
                                                                           
        // Gigatron native instructions                                    
        else if(token == ".LD")   {instructionType._opcode = 0x00; instructionType._byteSize = TwoBytes; instructionType._opcodeType = Native;}
        else if(token == ".NOP")  {instructionType._opcode = 0x02; instructionType._byteSize = TwoBytes; instructionType._opcodeType = Native;}
        else if(token == ".ANDA") {instructionType._opcode = 0x20; instructionType._byteSize = TwoBytes; instructionType._opcodeType = Native;}
        else if(token == ".ORA")  {instructionType._opcode = 0x40; instructionType._byteSize = TwoBytes; instructionType._opcodeType = Native;}
        else if(token == ".XORA") {instructionType._opcode = 0x60; instructionType._byteSize = TwoBytes; instructionType._opcodeType = Native;}
        else if(token == ".ADDA") {instructionType._opcode = 0x80; instructionType._byteSize = TwoBytes; instructionType._opcodeType = Native;}
        else if(token == ".SUBA") {instructionType._opcode = 0xA0; instructionType._byteSize = TwoBytes; instructionType._opcodeType = Native;}
        else if(token == ".ST")   {instructionType._opcode = 0xC0; instructionType._byteSize = TwoBytes; instructionType._opcodeType = Native;}
        else if(token == ".JMP")  {instructionType._opcode = 0xE0; instructionType._byteSize = TwoBytes; instructionType._opcodeType = Native;}
        else if(token == ".BGT")  {instructionType._opcode = 0xE4; instructionType._byteSize = TwoBytes; instructionType._opcodeType = Native;}
        else if(token == ".BLT")  {instructionType._opcode = 0xE8; instructionType._byteSize = TwoBytes; instructionType._opcodeType = Native;}
        else if(token == ".BNE")  {instructionType._opcode = 0xEC; instructionType._byteSize = TwoBytes; instructionType._opcodeType = Native;}
        else if(token == ".BEQ")  {instructionType._opcode = 0xF0; instructionType._byteSize = TwoBytes; instructionType._opcodeType = Native;}
        else if(token == ".BGE")  {instructionType._opcode = 0xF4; instructionType._byteSize = TwoBytes; instructionType._opcodeType = Native;}
        else if(token == ".BLE")  {instructionType._opcode = 0xF8; instructionType._byteSize = TwoBytes; instructionType._opcodeType = Native;}
        else if(token == ".BRA")  {instructionType._opcode = 0xFC; instructionType._byteSize = TwoBytes; instructionType._opcodeType = Native;}

        return instructionType;
    }

    void preProcessExpression(const std::vector<std::string>& tokens, int tokenIndex, std::string& input, bool stripWhiteSpace)
    {
        input.clear();

        // Pre-process
        for(int j=tokenIndex; j<tokens.size(); j++)
        {
            // Strip comments
            if(tokens[j].find_first_of(";#") != std::string::npos) break;

            // Concatenate
            input += tokens[j];
        }

        // Strip white space
        if(stripWhiteSpace) input.erase(remove_if(input.begin(), input.end(), isspace), input.end());
    }

    size_t findSymbol(const std::string& input, const std::string& symbol, size_t pos = 0)
    {
        const char* separators = "+-*/().,!?;#'\"[] \t\n\r";
        const size_t len = input.length();
        if (pos >= len)
            return std::string::npos;
        for(;;) {
            size_t sep = input.find_first_of(separators, pos);
            bool eos = (sep == std::string::npos);
            size_t end = eos ? len : sep;
            if (input.substr(pos, end-pos) == symbol)
                return pos;
            else if (eos)
                return std::string::npos;
            pos = sep+1;
        }
        return std::string::npos;   // unreachable
    }

    bool applyEquatesToExpression(std::string& expression, const std::vector<Equate>& equates)
    {
        bool modified = false;
        for(int i=0; i<equates.size(); i++) {
            for (;;) {
                size_t pos = findSymbol(expression, equates[i]._name);
                if (pos == std::string::npos)
                    break;  // not found
                modified = true;
                expression.replace(pos, equates[i]._name.size(), std::to_string(equates[i]._operand));
            }
        }
        return modified;
    }

    bool applyLabelsToExpression(std::string& expression, const std::vector<Label>& labels, bool nativeCode)
    {
        bool modified = false;
        for(int i=0; i<labels.size(); i++) {
            for (;;) {
                size_t pos = findSymbol(expression, labels[i]._name);
                if (pos == std::string::npos)
                    break;  // not found
                modified = true;
                uint16_t address = (nativeCode) ? labels[i]._address >>1 : labels[i]._address;
                expression.replace(pos, labels[i]._name.size(), std::to_string(address));
            }
        }
        return modified;
    }

    uint16_t evaluateExpression(std::string input, bool nativeCode)
    { 
        // Replace equates
        applyEquatesToExpression(input, _equates);

        // Replace labels
        applyLabelsToExpression(input, _labels, nativeCode);

        // Strip white space
        input.erase(remove_if(input.begin(), input.end(), isspace), input.end());

        // Parse expression and return with a result
        return Expression::parse((char*)input.c_str(), _lineNumber);
    }

    bool searchEquate(const std::string& token, Equate& equate)
    {
        bool success = false;
        for(int i=0; i<_equates.size(); i++)
        {
            if(_equates[i]._name == token)
            {
                equate = _equates[i];
                success = true;
                break;
            }
        }

        return success;
    }

    bool evaluateEquateOperand(const std::string& token, Equate& equate)
    {
        // Expression equates
        Expression::ExpressionType expressionType = Expression::isExpression(token);
        if(expressionType == Expression::Invalid) return false;
        if(expressionType == Expression::Valid)
        {
            equate._operand = evaluateExpression(token, false);
            return true;
        }

        // Check for existing equate
        return searchEquate(token, equate);
    }

    bool evaluateEquateOperand(const std::vector<std::string>& tokens, int tokenIndex, Equate& equate, bool compoundInstruction)
    {
        if(tokenIndex >= tokens.size()) return false;

        // Expression equates
        std::string token;
        if(compoundInstruction)
        {
            token = tokens[tokenIndex];
        }
        else
        {
            preProcessExpression(tokens, tokenIndex, token, false);
        }

        return evaluateEquateOperand(token, equate);
    }

    EvaluateResult evaluateEquates(const std::vector<std::string>& tokens, ParseType parse)
    {
        if(tokens[1] == "EQU"  ||  tokens[1] == "equ")
        {
            if(parse == MnemonicPass)
            {
                Equate equate = {false, 0x0000, tokens[0]};
                if(!Expression::stringToU16(tokens[2], equate._operand))
                {
                    if(!evaluateEquateOperand(tokens, 2, equate, false)) return NotFound;
                }

                // Reserved word, (equate), _callTable_
                if(tokens[0] == "_callTable_")
                {
                    _callTable = equate._operand;
                }
                // Reserved word, (equate), _startAddress_
                else if(tokens[0] == "_startAddress_")
                {
                    _startAddress = equate._operand;
                    _currentAddress = _startAddress;
                }
#ifndef STAND_ALONE
                // Disable upload of the current assembler module
                else if(tokens[0] == "_disableUpload_")
                {
                    Loader::disableUploads(equate._operand != 0);
                }
                // Reserved word, (equate), _singleStepWatch_
                else if(tokens[0] == "_singleStepWatch_")
                {
                    Editor::setSingleStepWatchAddress(equate._operand);
                }
                // Start address of vCPU exclusion zone
                else if(tokens[0] == "_cpuUsageAddressA_")
                {
                    Editor::setCpuUsageAddressA(equate._operand);
                }
                // End address of vCPU exclusion zone
                else if(tokens[0] == "_cpuUsageAddressB_")
                {
                    Editor::setCpuUsageAddressB(equate._operand);
                }
#endif
                // Standard equates
                else
                {
                    // Check for duplicate
                    equate._name = tokens[0];
                    if(searchEquate(tokens[0], equate)) return Duplicate;

                    _equates.push_back(equate);
                }
            }
            else if(parse == CodePass)
            {
            }

            return Success;
        }

        return Failed;
    }

    bool searchLabel(const std::string& token, Label& label)
    {
        bool success = false;
        for(int i=0; i<_labels.size(); i++)
        {
            if(token == _labels[i]._name)
            {
                success = true;
                label = _labels[i];
                break;
            }
        }

        return success;
    }

    bool evaluateLabelOperand(const std::string& token, Label& label)
    {
        // Expression labels
        Expression::ExpressionType expressionType = Expression::isExpression(token);
        if(expressionType == Expression::Invalid) return false;
        if(expressionType == Expression::Valid)
        {
            label._address = evaluateExpression(token, false);
            return true;
        }

        // Check for existing label
        return searchLabel(token, label);
    }

    bool evaluateLabelOperand(const std::vector<std::string>& tokens, int tokenIndex, Label& label, bool compoundInstruction)
    {
        if(tokenIndex >= tokens.size()) return false;

        // Expression labels
        std::string token;
        if(compoundInstruction)
        {
            token = tokens[tokenIndex];
        }
        else
        {
            preProcessExpression(tokens, tokenIndex, token, false);
        }

        return evaluateLabelOperand(token, label);
    }

    EvaluateResult EvaluateLabels(const std::vector<std::string>& tokens, ParseType parse, int tokenIndex)
    {
        if(parse == MnemonicPass) 
        {
            // Check reserved words
            for(int i=0; i<_reservedWords.size(); i++)
            {
                if(tokens[tokenIndex] == _reservedWords[i]) return Reserved;
            }
            
            Label label;
            if(searchLabel(tokens[tokenIndex], label)) return Duplicate;

            // Check equates for a custom start address
            for(int i=0; i<_equates.size(); i++)
            {
                if(_equates[i]._name == tokens[tokenIndex])
                {
                    _equates[i]._isCustomAddress = true;
                    _currentAddress = _equates[i]._operand;
                    break;
                }
            }

            // Normal labels
            label = {_currentAddress, tokens[tokenIndex]};
            _labels.push_back(label);
        }
        else if(parse == CodePass)
        {
        }

        return Success;
    }

    bool handleDefineByte(const std::vector<std::string>& tokens, int tokenIndex, const Instruction& instruction, bool createInstruction, int& dbSize)
    {
        bool success = false;

        // Handle case where first operand is a string
        size_t quote1 = tokens[tokenIndex].find_first_of("'\"");
        size_t quote2 = tokens[tokenIndex].find_first_of("'\"", quote1+1);
        bool quotes = (quote1 != std::string::npos  &&  quote2 != std::string::npos  &&  (quote2 - quote1 > 1));
        if(quotes)
        {
            std::string token = tokens[tokenIndex].substr(quote1+1, quote2 - (quote1+1));
            if(createInstruction)
            {
                for(int j=1; j<token.size(); j++) // First instruction was created by callee
                {
                    Instruction inst = {instruction._isRomAddress, false, OneByte, uint8_t(token[j]), 0x00, 0x00, 0x0000, instruction._opcodeType};
                    _instructions.push_back(inst);
                }
            }
            dbSize += int(token.size()) - 1; // First instruction was created by callee
            success = true;
        }
       
        for(int i=tokenIndex+1; i<tokens.size(); i++)
        {
            // Handle all other variations of strings
            size_t quote1 = tokens[i].find_first_of("'\"");
            size_t quote2 = tokens[i].find_first_of("'\"", quote1+1);
            bool quotes = (quote1 != std::string::npos  &&  quote2 != std::string::npos);
            if(quotes)
            {
                std::string token = tokens[i].substr(quote1+1, quote2 - (quote1+1));
                if(createInstruction)
                {
                    for(int j=0; j<token.size(); j++)
                    {
                        Instruction inst = {instruction._isRomAddress, false, OneByte, uint8_t(token[j]), 0x00, 0x00, 0x0000, instruction._opcodeType};
                        _instructions.push_back(inst);
                    }
                }
                dbSize += int(token.size());
                success = true;
            }
            // Non string tokens
            else
            {
                // Strip comments
                if(tokens[i].find_first_of(";#") != std::string::npos) break;

                uint8_t operand;
                success = Expression::stringToU8(tokens[i], operand);
                if(!success)
                {
                    // Search equates
                    Equate equate;
                    Label label;
                    if(success = evaluateEquateOperand(tokens[i], equate))
                    {
                        operand = uint8_t(equate._operand);
                    }
                    // Search labels
                    else if(success = evaluateLabelOperand(tokens[i], label))
                    {
                        operand = uint8_t(label._address);
                    }
                    else
                    {
                        // Normal expression
                        if(Expression::isExpression(tokens[i]) == Expression::Valid)
                        {
                            operand = uint8_t(Expression::parse((char*)tokens[i].c_str(), _lineNumber));
                            success = true;
                        }
                        else
                        {
                            break;
                        }
                    }
                }

                if(createInstruction)
                {
                    Instruction inst = {instruction._isRomAddress, false, OneByte, operand, 0x00, 0x00, 0x0000, instruction._opcodeType};
                    _instructions.push_back(inst);
                }
                dbSize++;
            }
        }

        return success;
    }


    bool handleDefineWord(const std::vector<std::string>& tokens, int tokenIndex, const Instruction& instruction, bool createInstruction, int& dwSize)
    {
        bool success = false;

        for(int i=tokenIndex+1; i<tokens.size(); i++)
        {
            // Strip comments
            if(tokens[i].find_first_of(";#") != std::string::npos)
            {
                success = true;
                break;
            }

            uint16_t operand;
            success = Expression::stringToU16(tokens[i], operand);
            if(!success)
            {
                // Search equates
                Equate equate;
                Label label;
                if(success = evaluateEquateOperand(tokens[i], equate))
                {
                    operand = equate._operand;
                }
                // Search labels
                else if(success = evaluateLabelOperand(tokens[i], label))
                {
                    operand = label._address;
                }
                else
                {
                    // Normal expression
                    if(Expression::isExpression(tokens[i]) == Expression::Valid)
                    {
                        operand = Expression::parse((char*)tokens[i].c_str(), _lineNumber);
                        success = true;
                    }
                    else
                    {
                        break;
                    }
                }
            }

            if(createInstruction)
            {
                Instruction inst = {instruction._isRomAddress, false, TwoBytes, uint8_t(operand & 0x00FF), uint8_t((operand & 0xFF00) >>8), 0x00, 0x0000,  instruction._opcodeType};
                _instructions.push_back(inst);
            }
            dwSize += 2;
        }

        return success;
    }

    bool handleNativeOperand(const std::string& token, uint8_t& operand)
    {
        Expression::ExpressionType expressionType = Expression::isExpression(token);
        if(expressionType == Expression::Invalid) return false;
        if(expressionType == Expression::Valid)
        {
            // Parse expression and return with a result
            operand = uint8_t(evaluateExpression(token, true));
            return true;
        }

        Label label;
        if(searchLabel(token, label))
        {
            operand = uint8_t((label._address >>1) & 0x00FF);
            return true;
        }

        Equate equate;
        if(searchEquate(token, equate))
        {
            operand = uint8_t(equate._operand);
            return true;
        }

        return Expression::stringToU8(token, operand);
    }

    bool handleNativeInstruction(const std::vector<std::string>& tokens, int tokenIndex, uint8_t& opcode, uint8_t& operand)
    {
        std::string input, token;;

        preProcessExpression(tokens, tokenIndex, input, true);
        size_t openBracket = input.find_first_of("[");
        size_t closeBracket = input.find_first_of("]");
        bool noBrackets = (openBracket == std::string::npos  &&  closeBracket == std::string::npos);
        bool validBrackets = (openBracket != std::string::npos  &&  closeBracket != std::string::npos  &&  closeBracket > openBracket);

        size_t comma1 = input.find_first_of(",");
        size_t comma2 = input.find_first_of(",", comma1+1);
        bool noCommas = (comma1 == std::string::npos  &&  comma2 == std::string::npos);
        bool oneComma = (comma1 != std::string::npos  &&  comma2 == std::string::npos);
        bool twoCommas = (comma1 != std::string::npos  &&  comma2 != std::string::npos);

        operand = 0x00;

        // NOP
        if(opcode == 0x02) return true;

        // Accumulator
        if(input == "AC"  ||  input == "ac")
        {
            opcode |= BusMode::AC;
            return true;
        }

        // Jump
        if(opcode == 0xE0)
        {
            // y,[D]
            if(validBrackets  &&  oneComma  &&  comma1 < openBracket)
            {
                opcode |= BusMode::RAM;
                token = input.substr(openBracket+1, closeBracket - (openBracket+1));
                return handleNativeOperand(token, operand);
            }

            // y,D
            if(noBrackets  &&  oneComma)
            {
                token = input.substr(comma1+1, input.size() - (comma1+1));
                return handleNativeOperand(token, operand);
            }
        
            return false;                    
        }

        // Branch
        if(opcode >= 0xE4)
        {
            token = input;
            if(validBrackets) {opcode |= BusMode::RAM; token = input.substr(openBracket+1, closeBracket - (openBracket+1));}
            if(Expression::stringToU8(token, operand)) return true;
            return handleNativeOperand(token, operand);
        }

        // IN or IN,[D]
        if(input.find("IN") != std::string::npos  ||  input.find("in") != std::string::npos)
        {
            opcode |= BusMode::IN;

            // IN,[D]
            if(validBrackets &&  oneComma  &&  comma1 < openBracket)
            {
                token = input.substr(openBracket+1, closeBracket - (openBracket+1));
                return handleNativeOperand(token, operand);
            }
            
            // IN
            return true;
        }

        // D
        if(noBrackets && noCommas) return handleNativeOperand(input, operand);

        // Read or write
        (opcode != 0xC0) ? opcode |= BusMode::RAM : opcode |= BusMode::AC;

        // [D] or [X]
        if(validBrackets  &&  noCommas)
        {
            token = input.substr(openBracket+1, closeBracket - (openBracket+1));
            if(token == "X"  ||  token == "x") {opcode |= AddressMode::X_AC; return true;}
            return handleNativeOperand(token, operand);
        }

        // AC,X or AC,Y or AC,OUT or D,X or D,Y or D,OUT or [D],X or [D],Y or [D],OUT or D,[D] or D,[X] or D,[Y] or [Y,D] or [Y,X] or [Y,X++]
        if(oneComma)
        {
            token = input.substr(comma1+1, input.size() - (comma1+1));
            if(token == "X"    ||  token == "x")   opcode |= AddressMode::D_X;
            if(token == "Y"    ||  token == "y")   opcode |= AddressMode::D_Y;
            if(token == "OUT"  ||  token == "out") opcode |= AddressMode::D_OUT;

            token = input.substr(0, comma1);

            // AC,X or AC,Y or AC,OUT
            if(token == "AC"  ||  token == "ac") {opcode &= 0xFC; opcode |= BusMode::AC; return true;}

            // D,X or D,Y or D,OUT
            if(noBrackets)
            {
                opcode &= 0xFC; return handleNativeOperand(token, operand);
            }

            // [D],X or [D],Y or [D],OUT or D,[D] or D,[X] or D,[Y] or [Y,D] or [Y,X] or [Y,X++]
            if(validBrackets)
            {
                if(comma1 > closeBracket) token = input.substr(openBracket+1, closeBracket - (openBracket+1));
                else if(comma1 < openBracket) {opcode &= 0xFC; token = input.substr(0, comma1);}
                else if(comma1 > openBracket  &&  comma1 < closeBracket)
                {
                    token = input.substr(openBracket+1, comma1 - (openBracket+1));
                    if(token != "Y"  &&  token != "y") return false;

                    token = input.substr(comma1+1, closeBracket - (comma1+1));
                    if(token == "X"    ||  token == "x")   {opcode |= AddressMode::YX_AC;    return true;}
                    if(token == "X++"  ||  token == "x++") {opcode |= AddressMode::YXpp_OUT; return true;}

                    opcode |= AddressMode::YD_AC;                
                }
                return handleNativeOperand(token, operand);
            }

            return false;
        }

        // D,[Y,X] or D,[Y,X++]
        if(validBrackets  &&  twoCommas  &&  comma1 < openBracket  &&  comma2 > openBracket  &&  comma2 < closeBracket)
        {
            token = input.substr(0, comma1);
            if(!handleNativeOperand(token, operand)) return false;

            token = input.substr(openBracket+1, comma2 - (openBracket+1));
            if(token != "Y"  &&  token != "y") return false;
            opcode &= 0xFC; // reset bus bits to D

            token = input.substr(comma2+1, closeBracket - (comma2+1));
            if(token == "X"    ||  token == "x")   {opcode |= YX_AC;    return true;}
            if(token == "X++"  ||  token == "x++") {opcode |= YXpp_OUT; return true;}
                
            return false;
        }

        // [Y,X++],out
        if(validBrackets  &&  twoCommas  &&  comma1 > openBracket  &&  comma2 > closeBracket)
        {
            token = input.substr(openBracket+1, comma1 - (openBracket+1));
            if(token != "Y"  &&  token != "y") return false;

            token = input.substr(comma1+1, closeBracket - (comma1+1));
            if(token == "X"    ||  token == "x")   {opcode |= YX_AC;    return true;}
            if(token == "X++"  ||  token == "x++") {opcode |= YXpp_OUT; return true;}
                
            return false;
        }

        return false;
    }

    void packByteCode(Instruction& instruction, ByteCode& byteCode)
    {
        switch(instruction._byteSize)
        {
            case OneByte:
            {
                byteCode._isRomAddress = instruction._isRomAddress;
                byteCode._isCustomAddress = instruction._isCustomAddress;
                byteCode._data = instruction._opcode;
                byteCode._address = instruction._address;
                _byteCode.push_back(byteCode);
            }
            break;

            case TwoBytes:
            {
                byteCode._isRomAddress = instruction._isRomAddress;
                byteCode._isCustomAddress = instruction._isCustomAddress;
                byteCode._data = instruction._opcode;
                byteCode._address = instruction._address;
                _byteCode.push_back(byteCode);

                byteCode._isRomAddress = instruction._isRomAddress;
                byteCode._isCustomAddress = false;
                byteCode._data = instruction._operand0;
                byteCode._address = 0x0000;
                _byteCode.push_back(byteCode);
            }
            break;

            case ThreeBytes:
            {
                byteCode._isRomAddress = instruction._isRomAddress;
                byteCode._isCustomAddress = instruction._isCustomAddress;
                byteCode._data = instruction._opcode;
                byteCode._address = instruction._address;
                _byteCode.push_back(byteCode);

                byteCode._isRomAddress = instruction._isRomAddress;
                byteCode._isCustomAddress = false;
                byteCode._data = instruction._operand0;
                byteCode._address = 0x0000;
                _byteCode.push_back(byteCode);

                byteCode._isRomAddress = instruction._isRomAddress;
                byteCode._isCustomAddress = false;
                byteCode._data = instruction._operand1;
                byteCode._address = 0x0000;
                _byteCode.push_back(byteCode);
            }
            break;
        }
    }

    void packByteCodeBuffer(void)
    {
        // Pack instructions
        ByteCode byteCode;
        uint16_t segmentOffset = 0x0000;
        uint16_t segmentAddress = 0x0000;
        for(int i=0; i<_instructions.size(); i++)
        {
            // Segment RAM instructions into 256 byte pages for .gt1 file format
            if(!_instructions[i]._isRomAddress)
            {
                // Save start of segment
                if(_instructions[i]._isCustomAddress)
                {
                    segmentOffset = 0x0000;
                    segmentAddress = _instructions[i]._address;
                }

                // Force a new segment, (this could fail if an instruction straddles a page boundary, but
                // the page boundary crossing detection logic will stop the assembler before we get here)
                if(!_instructions[i]._isCustomAddress  &&  segmentOffset % 256 == 0)
                {
                    _instructions[i]._isCustomAddress = true;
                    _instructions[i]._address = segmentAddress + segmentOffset;
                }

                segmentOffset += _instructions[i]._byteSize;
            }

            packByteCode(_instructions[i], byteCode);
        }

        // Append call table
        if(_callTable  &&  _callTableEntries.size())
        {
            // _callTable grows downwards, pointer is 2 bytes below the bottom of the table by the time we get here
            for(int i=int(_callTableEntries.size())-1; i>=0; i--)
            {
                int end = int(_callTableEntries.size()) - 1;
                byteCode._isRomAddress = false;
                byteCode._isCustomAddress = (i == end) ?  true : false;
                byteCode._data = _callTableEntries[i]._address & 0x00FF;
                byteCode._address = _callTable + (end-i)*2 + 2;
                _byteCode.push_back(byteCode);

                byteCode._isRomAddress = false;
                byteCode._isCustomAddress = false;
                byteCode._data = (_callTableEntries[i]._address & 0xFF00) >>8;
                byteCode._address = _callTable + (end-i)*2 + 3;
                _byteCode.push_back(byteCode);
            }
        }
    }

    bool checkInvalidAddress(ParseType parse, uint16_t currentAddress, uint16_t instructionSize, const Instruction& instruction, const LineToken& lineToken, const std::string& filename, int lineNumber)
    {
        // Check for audio channel stomping
        if(parse == CodePass  &&  !instruction._isRomAddress)
        {
            uint16_t start = currentAddress;
            uint16_t end = currentAddress + instructionSize - 1;
            if((start >= GIGA_CH0_WAV_A  &&  start <= GIGA_CH0_OSC_H)  ||  (end >= GIGA_CH0_WAV_A  &&  end <= GIGA_CH0_OSC_H)  ||
               (start >= GIGA_CH1_WAV_A  &&  start <= GIGA_CH1_OSC_H)  ||  (end >= GIGA_CH1_WAV_A  &&  end <= GIGA_CH1_OSC_H)  ||
               (start >= GIGA_CH2_WAV_A  &&  start <= GIGA_CH2_OSC_H)  ||  (end >= GIGA_CH2_WAV_A  &&  end <= GIGA_CH2_OSC_H)  ||
               (start >= GIGA_CH3_WAV_A  &&  start <= GIGA_CH3_OSC_H)  ||  (end >= GIGA_CH3_WAV_A  &&  end <= GIGA_CH3_OSC_H))
            {
                fprintf(stderr, "Assembler::assemble() : Warning, audio channel boundary compromised : 0x%04X <-> 0x%04X\nAssembler::assemble() : '%s'\nAssembler::assemble() : in '%s' on line %d\n", start, end, lineToken._text.c_str(), filename.c_str(), lineNumber+1);
            }
        }

        // Check for page boundary crossings
        if(parse == CodePass  &&  (instruction._opcodeType == vCpu || instruction._opcodeType == Native))
        {
            static uint16_t customAddress = 0x0000;
            if(instruction._isCustomAddress) customAddress = instruction._address;

            uint16_t oldAddress = (instruction._isRomAddress) ? customAddress + ((currentAddress & 0x00FF)>>1) : currentAddress;
            currentAddress += instructionSize - 1;
            uint16_t newAddress = (instruction._isRomAddress) ? customAddress + ((currentAddress & 0x00FF)>>1) : currentAddress;
            if((oldAddress >>8) != (newAddress >>8))
            {
                fprintf(stderr, "Assembler::assemble() : Page boundary compromised : %04X : %04X : '%s' : in '%s' on line %d\n", oldAddress, newAddress, lineToken._text.c_str(), filename.c_str(), lineNumber+1);
                return false;
            }
        }

        return true;
    }

    std::vector<std::string> tokenise(const std::string& text, char c)
    {
        std::vector<std::string> result;
        const char* str = text.c_str();

        do
        {
            const char *begin = str;

            while(*str  &&  *str != c) str++;

            if(str > begin) result.push_back(std::string(begin, str));
        }
        while (*str++ != 0);

        return result;
    }

    std::vector<std::string> tokeniseLine(std::string& line)
    {
        std::string token = "";
        bool delimiterStart = true;
        bool stringStart = false;
        enum DelimiterState {WhiteSpace, Quotes};
        DelimiterState delimiterState = WhiteSpace;
        std::vector<std::string> tokens;

        for(int i=0; i<=line.size(); i++)
        {
            // End of line is a delimiter for white space
            if(i == line.size())
            {
                if(delimiterState != Quotes)
                {
                    delimiterState = WhiteSpace;
                    delimiterStart = false;
                }
                else
                {
                    break;
                }
            }
            else
            {
                // White space delimiters
                if(strchr(" \n\r\f\t\v", line[i]))
                {
                    if(delimiterState != Quotes)
                    {
                        delimiterState = WhiteSpace;
                        delimiterStart = false;
                    }
                }
                // String delimiters
                else if(strchr("\'\"", line[i]))
                {
                    delimiterState = Quotes;
                    stringStart = !stringStart;
                }
            }

            // Build token
            switch(delimiterState)
            {
                case WhiteSpace:
                {
                    // Don't save delimiters
                    if(delimiterStart)
                    {
                        if(!strchr(" \n\r\f\t\v", line[i])) token += line[i];
                    }
                    else
                    {
                        if(token.size()) tokens.push_back(token);
                        delimiterStart = true;
                        token = "";
                    }
                }
                break;

                case Quotes:
                {
                    // Save delimiters as well as chars
                    if(stringStart)
                    {
                        token += line[i];
                    }
                    else
                    {
                        token += line[i];
                        tokens.push_back(token);
                        delimiterState = WhiteSpace;
                        stringStart = false;
                        token = "";
                    }
                }
                break;
            }
        }

        return tokens;
    }

    bool handleInclude(const std::vector<std::string>& tokens, const std::string& lineToken, int lineIndex, std::vector<LineToken>& includeLineTokens)
    {
        // Check include syntax
        if(tokens.size() != 2)
        {
            fprintf(stderr, "Assembler::handleInclude() : Bad %%include statement : '%s' : on line %d\n", lineToken.c_str(), lineIndex);
            return false;
        }

        std::string filepath = _includePath + tokens[1];
        std::replace( filepath.begin(), filepath.end(), '\\', '/');
        std::ifstream infile(filepath);
        if(!infile.is_open())
        {
            fprintf(stderr, "Assembler::handleInclude() : Failed to open file : '%s'\n", filepath.c_str());
            return false;
        }

        // Collect lines from include file
        int lineNumber = lineIndex;
        while(!infile.eof())
        {
            LineToken includeLineToken = {true, lineNumber++ - lineIndex, "", filepath};
            std::getline(infile, includeLineToken._text);
            includeLineTokens.push_back(includeLineToken);

            if(!infile.good() &&  !infile.eof())
            {
                fprintf(stderr, "Assembler::handleInclude() : Bad lineToken : '%s' : in '%s' on line %d\n", includeLineToken._text.c_str(), filepath.c_str(), lineNumber - lineIndex);
                return false;
            }
        }

        return true;
    }

    bool handleMacros(const std::vector<Macro>& macros, std::vector<LineToken>& lineTokens)
    {
        // Incomplete macros
        for(int i=0; i<macros.size(); i++)
        {
            if(!macros[i]._complete)
            {
                fprintf(stderr, "Assembler::handleMacros() : Bad macro : missing 'ENDM' : in '%s' : on line %d\n", macros[i]._filename.c_str(), macros[i]._fileStartLine);
                return false;
            }
        }

        // Delete original macros
        int prevMacrosSize = 0;
        for(int i=0; i<macros.size(); i++)
        {
            lineTokens.erase(lineTokens.begin() + macros[i]._startLine - prevMacrosSize, lineTokens.begin() + macros[i]._endLine + 1 - prevMacrosSize);
            prevMacrosSize += macros[i]._endLine - macros[i]._startLine + 1;
        }

        // Find and expand macro
        int macroInstanceId = 0;
        for(int m=0; m<macros.size(); m++)
        {
            bool macroMissing = true;
            bool macroMissingParams = true;
            Macro macro = macros[m];

            for(auto itLine=lineTokens.begin(); itLine!=lineTokens.end();)
            {
                // Lines containing only white space are skipped
                LineToken lineToken = *itLine;
                size_t nonWhiteSpace = lineToken._text.find_first_not_of("  \n\r\f\t\v");
                if(nonWhiteSpace == std::string::npos)
                {
                    ++itLine;
                    continue;
                }

                // Tokenise current line
                std::vector<std::string> tokens = tokeniseLine(lineToken._text);

                // Find macro
                bool macroSuccess = false;
                for(int t=0; t<tokens.size(); t++)
                {
                    if(tokens[t] == macro._name)
                    {
                        macroMissing = false;
                        if(tokens.size() - t > macro._params.size())
                        {
                            macroMissingParams = false;
                            std::vector<std::string> labels;
                            std::vector<LineToken> macroLines;

                            // Create substitute lines
                            for(int ml=0; ml<macro._lines.size(); ml++)
                            {
                                // Tokenise macro line
                                std::vector<std::string> mtokens =  tokeniseLine(macro._lines[ml]);

                                // Save labels
                                size_t nonWhiteSpace = macro._lines[ml].find_first_not_of("  \n\r\f\t\v");
                                if(nonWhiteSpace == 0) labels.push_back(mtokens[0]);

                                // Replace parameters
                                for(int mt=0; mt<mtokens.size(); mt++)
                                {
                                    for(int p=0; p<macro._params.size(); p++)
                                    {
                                        if(mtokens[mt] == macro._params[p]) mtokens[mt] = tokens[t + 1 + p];
                                    }
                                }

                                // New macro line using any existing label
                                LineToken macroLine = {false, 0, "", ""};
                                macroLine._text = (t > 0  &&  ml == 0) ? tokens[0] : "";

                                // Append to macro line
                                for(int mt=0; mt<mtokens.size(); mt++)
                                {
                                    // Don't prefix macro labels with a space
                                    if(nonWhiteSpace != 0  ||  mt != 0) macroLine._text += " ";

                                    macroLine._text += mtokens[mt];
                                }

                                macroLines.push_back(macroLine);
                            }

                            // Insert substitute lines
                            for(int ml=0; ml<macro._lines.size(); ml++)
                            {
                                // Delete macro caller
                                if(ml == 0) itLine = lineTokens.erase(itLine);

                                // Each instance of a macro's labels are made unique
                                for(int i=0; i<labels.size(); i++)
                                {
                                    size_t labelFoundPos = macroLines[ml]._text.find(labels[i]);
                                    if(labelFoundPos != std::string::npos) macroLines[ml]._text.insert(labelFoundPos + labels[i].size(), std::to_string(macroInstanceId));
                                }

                                // Insert macro lines
                                itLine = lineTokens.insert(itLine, macroLines[ml]);
                                ++itLine;
                            }

                            macroInstanceId++;
                            macroSuccess = true;
                        }
                    }
                }

                if(!macroSuccess) ++itLine;
            }

            if(macroMissing)
            {
                fprintf(stderr, "Assembler::handleMacros() : Warning, macro is never called : '%s' : in '%s' : on line %d\n", macro._name.c_str(), macro._filename.c_str(), macro._fileStartLine);
                continue;
            }

            if(macroMissingParams)
            {
                fprintf(stderr, "Assembler::handleMacros() : Missing macro parameters : '%s' : in '%s' : on line %d\n", macro._name.c_str(), macro._filename.c_str(), macro._fileStartLine);
                return false;
            }
        }

        return true;
    }

    bool handleMacroStart(const std::string& filename, const LineToken& lineToken, const std::vector<std::string>& tokens, Macro& macro, int lineIndex, int adjustedLineIndex)
    {
        int lineNumber = (lineToken._fromInclude) ? lineToken._includeLineNumber + 1 : adjustedLineIndex + 1;
        std::string macroFileName = (lineToken._fromInclude) ? lineToken._includeName : filename;

        // Check macro syntax
        if(tokens.size() < 2)
        {
            fprintf(stderr, "Assembler::handleMacroStart() : Bad macro : missing name : in '%s' : on line %d\n", macroFileName.c_str(), lineNumber);
            return false;
        }                    

        macro._name = tokens[1];
        macro._startLine = lineIndex - 1;
        macro._fromInclude = lineToken._fromInclude;
        macro._fileStartLine = lineNumber;
        macro._filename = macroFileName;

        // Save params
        for(int i=2; i<tokens.size(); i++) macro._params.push_back(tokens[i]);

        return true;
    }

    bool handleMacroEnd(std::vector<Macro>& macros, Macro& macro, int lineIndex)
    {
        // Check for duplicates
        for(int i=0; i<macros.size(); i++)
        {
            if(macro._name == macros[i]._name)
            {
                fprintf(stderr, "Assembler::handleMacroEnd() : Bad macro : duplicate name : '%s' : in '%s' : on line %d\n", macro._name.c_str(), macro._filename.c_str(), macro._fileStartLine);
                return false;
            }
        }
        macro._endLine = lineIndex - 1;
        macro._complete = true;
        macros.push_back(macro);

        macro._name = "";
        macro._startLine = 0;
        macro._endLine = 0;
        macro._lines.clear();
        macro._params.clear();
        macro._complete = false;

        return true;
    }

    bool preProcess(const std::string& filename, std::vector<LineToken>& lineTokens, bool doMacros)
    {
        Macro macro;
        std::vector<Macro> macros;
        bool buildingMacro = false;

        int adjustedLineIndex = 0;
        for(auto itLine=lineTokens.begin(); itLine != lineTokens.end();)
        {
            // Lines containing only white space are skipped
            LineToken lineToken = *itLine;
            size_t nonWhiteSpace = lineToken._text.find_first_not_of("  \n\r\f\t\v");
            if(nonWhiteSpace == std::string::npos)
            {
                ++itLine;
                ++adjustedLineIndex;
                continue;
            }

            int tokenIndex = 0;
            bool includeFound = false;
            int lineIndex = int(itLine - lineTokens.begin()) + 1;

            // Tokenise current line
            std::vector<std::string> tokens = tokeniseLine(lineToken._text);

            // Valid pre-processor commands
            if(tokens.size() > 0)
            {
                Expression::strToUpper(tokens[0]);

                // Include
                if(tokens[0] == "%INCLUDE")
                {  
                    std::vector<LineToken> includeLineTokens;
                    if(!handleInclude(tokens, lineToken._text, lineIndex, includeLineTokens)) return false;

                    // Recursively include everything in order
                    if(!preProcess(filename, includeLineTokens, false))
                    {
                        fprintf(stderr, "Assembler::preProcess() : Bad include file : '%s'\n", tokens[1].c_str());
                        return false;
                    }

                    // Remove original include line and replace with include text
                    itLine = lineTokens.erase(itLine);
                    itLine = lineTokens.insert(itLine, includeLineTokens.begin(), includeLineTokens.end());
                    ++adjustedLineIndex -= int(includeLineTokens.end() - includeLineTokens.begin());
                    includeFound = true;
                }
                // Build macro
                else if(doMacros)
                {
                    if(tokens[0] == "%MACRO")
                    {
                        if(!handleMacroStart(filename, lineToken, tokens, macro, lineIndex, adjustedLineIndex)) return false;

                        buildingMacro = true;
                    }
                    else if(buildingMacro  &&  tokens[0] == "%ENDM")
                    {
                        if(!handleMacroEnd(macros, macro, lineIndex)) return false;
                        buildingMacro = false;
                    }
                    if(buildingMacro  &&  tokens[0] != "%MACRO")
                    {
                        macro._lines.push_back(lineToken._text);
                    }
                }
            }

            if(!includeFound)
            {
                ++itLine;
                ++adjustedLineIndex;
            }
        }

        // Handle complete macros
        if(doMacros  &&  !handleMacros(macros, lineTokens)) return false;

        return true;
    }

    bool parseGprintfFormat(const std::string& format, const std::vector<std::string>& variables, std::vector<Gprintf::Var>& vars, std::vector<std::string>& subs)
    {
        const char* fmt = format.c_str();
        std::string sub;
        char chr;

        int width = 0, index = 0;
        bool foundToken = false;

        while(chr = *fmt++)
        {
            if(index + 1 > variables.size()) return false;

            if(chr == '%'  ||  foundToken)
            {
                foundToken = true;
                Gprintf::Type type = Gprintf::Int;
                sub += chr;

                switch(chr)
                {
                    case '0':
                    {
                        // Maximum field width of 16 digits
                        width = strtol(fmt, nullptr, 10) % 17;
                    }
                    break;

                    case 'c': type = Gprintf::Chr; break;
                    case 'd': type = Gprintf::Int; break;
                    case 'b': type = Gprintf::Bin; break;
                    case 'q':
                    case 'o': type = Gprintf::Oct; break;
                    case 'x': type = Gprintf::Hex; break;
                    case 's': type = Gprintf::Str; break;
                }

                if(chr == 'c' || chr == 'd' || chr == 'b' || chr == 'q' || chr == 'o' || chr == 'x' || chr == 's')
                {
                    foundToken = false;
                    Gprintf::Var var = {false, type, width, 0x0000, variables[index++]};
                    vars.push_back(var);
                    subs.push_back(sub);
                    sub.clear();
                    width = 0;
                }
            }
        }

        return true;
    }

    bool createGprintf(ParseType parse, const std::string& lineToken, int lineNumber)
    {
        std::string input = lineToken;
        Expression::strToUpper(input);

        if(input.find("GPRINTF") != std::string::npos)
        {
            size_t openBracket = lineToken.find_first_of("(");
            size_t closeBracket = lineToken.find_first_of(")", openBracket+1);
            bool brackets = (openBracket != std::string::npos  &&  closeBracket != std::string::npos  &&  (closeBracket - openBracket > 2));

            if(brackets)
            {
                size_t quote1 = lineToken.find_first_of("\"", openBracket+1);
                size_t quote2 = lineToken.find_first_of("\"", quote1+1);
                bool quotes = (quote1 != std::string::npos  &&  quote2 != std::string::npos  &&  (quote2 - quote1 > 0));

                if(quotes)
                {
                    if(parse == MnemonicPass)
                    {
                        std::string formatText = lineToken.substr(quote1+1, quote2 - (quote1+1));
                        std::string variableText = lineToken.substr(quote2+1, closeBracket - (quote2+1));

                        std::vector<Gprintf::Var> vars;
                        std::vector<std::string> subs;
                        std::vector<std::string> variables = tokenise(variableText, ',');
                        parseGprintfFormat(formatText, variables, vars, subs);

                        Gprintf gprintf = {false, _currentAddress, lineNumber, lineToken, formatText, vars, subs};
                        _gprintfs.push_back(gprintf);
                    }

                    return true;
                }
            }

            fprintf(stderr, "Assembler::createGprintf() : Bad gprintf format : '%s' : on line %d\n", lineToken.c_str(), lineNumber);
            return false;
        }

        return false;
    }

    bool parseGprintfs(void)
    {
        for(int i = 0; i<_gprintfs.size(); i++)
        {
            for(int j = 0; j<_gprintfs[i]._vars.size(); j++)
            {
                bool success = false;
                uint16_t data = 0x0000;
                std::string token = _gprintfs[i]._vars[j]._var;
        
                // Strip white space
                token.erase(remove_if(token.begin(), token.end(), isspace), token.end());
                _gprintfs[i]._vars[j]._var = token;

                // Check for indirection
                size_t asterisk = token.find_first_of("*");
                if(asterisk != std::string::npos)
                {
                    _gprintfs[i]._vars[j]._indirect = true;
                    token = token.substr(asterisk+1);
                }

                success = Expression::stringToU16(token, data);
                if(!success)
                {
                    std::vector<std::string> tokens;
                    tokens.push_back(token);

                    // Search equates
                    Equate equate;
                    Label label;
                    if(success = evaluateEquateOperand(tokens, 0, equate, false))
                    {
                        data = equate._operand;
                    }
                    // Search labels
                    else if(success = evaluateLabelOperand(tokens, 0, label, false))
                    {
                        data = label._address;
                    }
                    // Normal expression
                    else
                    {
                        if(Expression::isExpression(token) == Expression::Valid)
                        {
                            data = Expression::parse((char*)token.c_str(), _lineNumber);
                            success = true;
                        }
                    }
                }

                if(!success)
                {
                    fprintf(stderr, "Assembler::parseGprintfs() : Error in gprintf(), missing label or equate : '%s' : in '%s' on line %d\n", token.c_str(), _gprintfs[i]._lineToken.c_str(), _gprintfs[i]._lineNumber);
                    _gprintfs.erase(_gprintfs.begin() + i);
                    return false;
                }

                _gprintfs[i]._vars[j]._data = data;
            }
        }

        return true;
    }

#ifndef STAND_ALONE
    bool getGprintfString(int index, std::string& gstring)
    {
        const Gprintf& gprintf = _gprintfs[index % _gprintfs.size()];
        gstring = gprintf._format;
   
        size_t subIndex = 0;
        for(int i=0; i<gprintf._vars.size(); i++)
        {
            char token[256];

            // Use indirection if required
            uint16_t data = (gprintf._vars[i]._indirect) ? Cpu::getRAM(gprintf._vars[i]._data) | (Cpu::getRAM(gprintf._vars[i]._data+1) <<8) : gprintf._vars[i]._data;
            
            // Maximum field width of 16 digits
            uint8_t width = gprintf._vars[i]._width % 17;
            std::string fieldWidth = "%";
            if(width) fieldWidth = std::string("%0" + std::to_string(width));

            switch(gprintf._vars[i]._type)
            {
                case Gprintf::Chr: fieldWidth += "c"; sprintf(token, fieldWidth.c_str(), data); break;
                case Gprintf::Int: fieldWidth += "d"; sprintf(token, fieldWidth.c_str(), data); break;
                case Gprintf::Oct: fieldWidth += "o"; sprintf(token, fieldWidth.c_str(), data); break;
                case Gprintf::Hex: fieldWidth += "x"; sprintf(token, fieldWidth.c_str(), data); break;

                // Strings are always indirect and assume that length is first byte
                case Gprintf::Str:
                {
                    data = gprintf._vars[i]._data;
                    uint8_t length = Cpu::getRAM(data) & 0xFF; // maximum length of 256
                    for(int j=0; j<length; j++) token[j] = Cpu::getRAM(data + j + 1);
                    token[length] = 0;
                }
                break;

                case Gprintf::Bin:
                {
                    for(int j=width-1; j>=0; j--)
                    {
                        token[width-1 - j] = '0' + ((data >> j) & 1);
                        if(j == 0) token[width-1 + 1] = 0;
                    }
                }
                break;
            }

            // Replace substrings
            subIndex = gstring.find(gprintf._subs[i], subIndex);
            if(subIndex != std::string::npos)
            {
                gstring.erase(subIndex, gprintf._subs[i].size());
                gstring.insert(subIndex, token);
            }
        }

        return true;
    }

    void printGprintfStrings(void)
    {
        if(_gprintfs.size())
        {
            uint16_t vPC = (Cpu::getRAM(0x0017) <<8) | Cpu::getRAM(0x0016);

            for(int i=0; i<_gprintfs.size(); i++)
            {
                if(vPC == _gprintfs[i]._address)
                {
                    // Emulator can cycle many times for one CPU cycle, so make sure gprintf is displayed only once
                    if(!_gprintfs[i]._displayed)
                    {
                        std::string gstring;
                        getGprintfString(i, gstring);
                        fprintf(stderr, "gprintf() : address $%04X : '%s'\n", _gprintfs[i]._address, gstring.c_str());
                        _gprintfs[i]._displayed = true;
                    }
                }
                else
                {
                    _gprintfs[i]._displayed = false;;
                }
            }
        }
    }
#endif

    void clearAssembler(void)
    {
        _byteCode.clear();
        _labels.clear();
        _equates.clear();
        _instructions.clear();
        _callTableEntries.clear();
        _gprintfs.clear();
    }

    bool assemble(const std::string& filename, uint16_t startAddress)
    {
        std::ifstream infile(filename);
        if(!infile.is_open())
        {
            fprintf(stderr, "Assembler::assemble() : Failed to open file : '%s'\n", filename.c_str());
            return false;
        }

        _callTable = 0x0000;
        _startAddress = startAddress;
        _currentAddress = _startAddress;
        clearAssembler();

#ifndef STAND_ALONE
        Loader::disableUploads(false);
#endif

        // Get file
        int numLines = 0;
        LineToken lineToken;
        std::vector<LineToken> lineTokens;
        while(!infile.eof())
        {
            std::getline(infile, lineToken._text);
            lineTokens.push_back(lineToken);

            if(!infile.good()  &&  !infile.eof())
            {
                fprintf(stderr, "Assembler::assemble() : Bad lineToken : '%s' : in '%s' : on line %d\n", lineToken._text.c_str(), filename.c_str(), numLines+1);
                return false;
            }

            numLines++;
        }

        // Pre-processor
        if(!preProcess(filename, lineTokens, true)) return false;

        numLines = int(lineTokens.size());

        // The mnemonic pass we evaluate all the equates and labels, the code pass is for the opcodes and operands
        for(int parse=MnemonicPass; parse<NumParseTypes; parse++)
        {
            for(_lineNumber=0; _lineNumber<numLines; _lineNumber++)
            {
                lineToken = lineTokens[_lineNumber];

                // Lines containing only white space are skipped
                size_t nonWhiteSpace = lineToken._text.find_first_not_of("  \n\r\f\t\v");
                if(nonWhiteSpace == std::string::npos) continue;

                int tokenIndex = 0;

                // Tokenise current line
                std::vector<std::string> tokens = tokeniseLine(lineToken._text);

                // Comments
                if(tokens.size() > 0  &&  tokens[0].find_first_of(";#") != std::string::npos) continue;

                // Gprintf lines are skipped
                if(createGprintf(ParseType(parse), lineToken._text, _lineNumber+1)) continue;

                // Starting address, labels and equates
                if(nonWhiteSpace == 0)
                {
                    if(tokens.size() >= 2)
                    {
                        EvaluateResult result = evaluateEquates(tokens, (ParseType)parse);
                        if(result == NotFound)
                        {
                            fprintf(stderr, "Assembler::assemble() : Missing equate : '%s' : in '%s' on line %d\n", lineToken._text.c_str(), filename.c_str(), _lineNumber+1);
                            return false;
                        }
                        else if(result == Duplicate)
                        {
                            fprintf(stderr, "Assembler::assemble() : Duplicate equate : '%s' : in '%s' on line %d\n", lineToken._text.c_str(), filename.c_str(), _lineNumber+1);
                            return false;
                        }
                        // Skip equate lines
                        else if(result == Success) 
                        {
                            continue;
                        }
                            
                        // Labels
                        result = EvaluateLabels(tokens, (ParseType)parse, tokenIndex);
                        if(result == Reserved)
                        {
                            fprintf(stderr, "Assembler::assemble() : Can't use a reserved word in a label : '%s' : in '%s' on line %d\n", tokens[tokenIndex].c_str(), filename.c_str(), _lineNumber+1);
                            return false;
                        }
                        else if(result == Duplicate)
                        {
                            fprintf(stderr, "Assembler::assemble() : Duplicate label : '%s' : in '%s' on line %d\n", lineToken._text.c_str(), filename.c_str(), _lineNumber+1);
                            return false;
                        }
                    }

                    // On to the next token even if we failed with this one
                    if(tokens.size() > 1) tokenIndex++;
                }

                // Opcode
                bool operandValid = false;
                InstructionType instructionType = getOpcode(tokens[tokenIndex++]);
                uint8_t opcode = instructionType._opcode;
                uint8_t branch = instructionType._branch;
                int outputSize = instructionType._byteSize;
                uint16_t additionalSize = 0;
                OpcodeType opcodeType = instructionType._opcodeType;
                Instruction instruction = {false, false, ByteSize(outputSize), opcode, 0x00, 0x00, _currentAddress, opcodeType};

                if(outputSize == BadSize)
                {
                    fprintf(stderr, "Assembler::assemble() : Bad Opcode : '%s' : in '%s' on line %d\n", lineToken._text.c_str(), filename.c_str(), _lineNumber+1);
                    return false;
                }

                // Compound instructions that require a Mnemonic pass
                bool compoundInstruction = false;
                if(opcodeType == ReservedDB  ||  opcodeType == ReservedDBR)
                {
                    compoundInstruction = true;
                    if(parse == MnemonicPass)
                    {
                        outputSize = OneByte; // first instruction has already been parsed
                        if(tokenIndex + 1 < tokens.size())
                        {
                            if(!handleDefineByte(tokens, tokenIndex, instruction, false, outputSize))
                            {
                                fprintf(stderr, "Assembler::assemble() : Bad DB data : '%s' : in '%s' on line %d\n", lineToken._text.c_str(), filename.c_str(), _lineNumber+1);
                                return false;
                            }
                        }
                    }
                }
                else if(opcodeType == ReservedDW  ||  opcodeType == ReservedDWR)
                {
                    compoundInstruction = true;
                    if(parse == MnemonicPass)
                    {
                        outputSize = TwoBytes; // first instruction has already been parsed
                        if(tokenIndex + 1 < tokens.size())
                        {
                            if(!handleDefineWord(tokens, tokenIndex, instruction, false, outputSize))
                            {
                                fprintf(stderr, "Assembler::assemble() : Bad DW data : '%s' : in '%s' on line %d\n", lineToken._text.c_str(), filename.c_str(), _lineNumber+1);
                                return false;
                            }
                        }
                    }
                }
                
                if(parse == CodePass)
                {
                    // Native NOP
                    if(opcodeType == Native  &&  opcode == 0x02)
                    {
                        operandValid = true;
                    }
                    // Missing operand
                    else if((outputSize == TwoBytes  ||  outputSize == ThreeBytes)  &&  tokens.size() <= tokenIndex)
                    {
                        fprintf(stderr, "Assembler::assemble() : Missing operand/s : '%s' : in '%s' on line %d\n", lineToken._text.c_str(), filename.c_str(), _lineNumber+1);
                        return false;
                    }

                    // First instruction inherits start address
                    if(_instructions.size() == 0)
                    {
                        instruction._address = _startAddress;
                        instruction._isCustomAddress = true;
                        _currentAddress = _startAddress;
                    }

                    // Custom address
                    for(int i=0; i<_equates.size(); i++)
                    {
                        if(_equates[i]._name == tokens[0]  &&  _equates[i]._isCustomAddress)
                        {
                            instruction._address = _equates[i]._operand;
                            instruction._isCustomAddress = true;
                            _currentAddress = _equates[i]._operand;
                        }
                    }

                    // Operand
                    switch(outputSize)
                    {
                        case OneByte:
                        {
                            _instructions.push_back(instruction);
                            if(!checkInvalidAddress(ParseType(parse), _currentAddress, outputSize, instruction, lineToken, filename, _lineNumber)) return false;
                        }
                        break;

                        case TwoBytes:
                        {
                            uint8_t operand = 0x00;

                            // BRA
                            if(opcodeType == vCpu  &&  opcode == 0x90)
                            {
                                // Search for branch label
                                Label label;
                                if(evaluateLabelOperand(tokens, tokenIndex, label, false))
                                {
                                    operandValid = true;
                                    operand = uint8_t(label._address) - BRANCH_ADJUSTMENT;
                                }
                                else
                                {
                                    fprintf(stderr, "Assembler::assemble() : Label missing : '%s' : in '%s' on line %d\n", tokens[tokenIndex].c_str(), filename.c_str(), _lineNumber+1);
                                    return false;
                                }
                            }
                            // CALL
                            else if(opcodeType == vCpu  &&  opcode == 0xCF  &&  _callTable)
                            {
                                // Search for call label
                                Label label;
                                if(evaluateLabelOperand(tokens, tokenIndex, label, false))
                                {
                                    // Search for address
                                    bool newLabel = true;
                                    uint16_t address = uint16_t(label._address);
                                    for(int i=0; i<_callTableEntries.size(); i++)
                                    {
                                        if(_callTableEntries[i]._address == address)
                                        {
                                            operandValid = true;
                                            operand = _callTableEntries[i]._operand;
                                            newLabel = false;
                                            break;
                                        }
                                    }

                                    // Found a new call address label, put it's address into the call table and point the call instruction to the call table
                                    if(newLabel)
                                    {
                                        operandValid = true;
                                        operand = uint8_t(_callTable & 0x00FF);
                                        CallTableEntry entry = {operand, address};
                                        _callTableEntries.push_back(entry);
                                        _callTable -= 0x0002;
                                    }
                                }
                                else
                                {
                                    fprintf(stderr, "Assembler::assemble() : Label missing : '%s' : in '%s' on line %d\n", tokens[tokenIndex].c_str(), filename.c_str(), _lineNumber+1);
                                    return false;
                                }
                            }
                                
                            // All other non native 2 byte instructions
                            if(opcodeType != Native  &&  !operandValid)
                            {
                                operandValid = Expression::stringToU8(tokens[tokenIndex], operand);
                                if(!operandValid)
                                {
                                    Label label;
                                    Equate equate;

                                    // String
                                    size_t quote1 = tokens[tokenIndex].find_first_of("'\"");
                                    size_t quote2 = tokens[tokenIndex].find_first_of("'\"", quote1+1);
                                    bool quotes = (quote1 != std::string::npos  &&  quote2 != std::string::npos  &&  (quote2 - quote1 > 1));
                                    if(quotes)
                                    {
                                        operand = uint8_t(tokens[tokenIndex][quote1+1]);
                                    }
                                    // Search equates
                                    else if(operandValid = evaluateEquateOperand(tokens, tokenIndex, equate, compoundInstruction))
                                    {
                                        operand = uint8_t(equate._operand);
                                    }
                                    // Search labels
                                    else if(operandValid = evaluateLabelOperand(tokens, tokenIndex, label, compoundInstruction))
                                    {
                                        operand = uint8_t(label._address);
                                    }
                                    else if(Expression::isExpression(tokens[tokenIndex]) == Expression::Valid)
                                    {
                                        std::string input;
                                        preProcessExpression(tokens, tokenIndex, input, true);
                                        operand = uint8_t(Expression::parse((char*)input.c_str(), _lineNumber));
                                        operandValid = true;
                                    }
                                    else if(!operandValid)
                                    {
                                        fprintf(stderr, "Assembler::assemble() : Label/Equate error : '%s' : in '%s' on line %d\n", tokens[tokenIndex].c_str(), filename.c_str(), _lineNumber+1);
                                        return false;
                                    }
                                }
                            }

                            // Native instructions
                            if(opcodeType == Native)
                            {
                                if(!operandValid)
                                {
                                    if(!handleNativeInstruction(tokens, tokenIndex, opcode, operand))
                                    {
                                        fprintf(stderr, "Assembler::assemble() : Native instruction is malformed : '%s' : in '%s' on line %d\n", lineToken._text.c_str(), filename.c_str(), _lineNumber+1);
                                        return false;
                                    }
                                }

                                instruction._isRomAddress = true;
                                instruction._opcode = opcode;
                                instruction._operand0 = uint8_t(operand & 0x00FF);
                                _instructions.push_back(instruction);
                                if(!checkInvalidAddress(ParseType(parse), _currentAddress, outputSize, instruction, lineToken, filename, _lineNumber)) return false;

#ifndef STAND_ALONE
                                uint16_t add = instruction._address>>1;
                                uint8_t opc = Cpu::getROM(add, 0);
                                uint8_t ope = Cpu::getROM(add, 1);
                                if(instruction._opcode != opc  ||  instruction._operand0 != ope)
                                {
                                    fprintf(stderr, "Assembler::assemble() : ROM Native instruction mismatch  : 0x%04X : ASM=0x%02X%02X : ROM=0x%02X%02X : on line %d\n", add, instruction._opcode, instruction._operand0, opc, ope, _lineNumber+1);

                                    // Fix mismatched instruction?
                                    //instruction._opcode = opc;
                                    //instruction._operand0 = ope;
                                    //_instructions.back() = instruction;
                                }
#endif
                            }
                            // Reserved assembler opcode DB, (define byte)
                            else if(opcodeType == ReservedDB  ||  opcodeType == ReservedDBR)
                            {
                                // Push first operand
                                outputSize = OneByte;
                                instruction._isRomAddress = (opcodeType == ReservedDBR) ? true : false;
                                instruction._byteSize = ByteSize(outputSize);
                                instruction._opcode = uint8_t(operand & 0x00FF);
                                _instructions.push_back(instruction);
    
                                // Push any remaining operands
                                if(tokenIndex + 1 < tokens.size())
                                {
                                    if(!handleDefineByte(tokens, tokenIndex, instruction, true, outputSize))
                                    {
                                        fprintf(stderr, "Assembler::assemble() : Bad DB data : '%s' : in '%s' on line %d\n", lineToken._text.c_str(), filename.c_str(), _lineNumber+1);
                                        return false;
                                    }
                                }

                                if(!checkInvalidAddress(ParseType(parse), _currentAddress, outputSize, instruction, lineToken, filename, _lineNumber)) return false;
                            }
                            // Normal instructions
                            else
                            {
                                instruction._operand0 = operand;
                                _instructions.push_back(instruction);
                                if(!checkInvalidAddress(ParseType(parse), _currentAddress, outputSize, instruction, lineToken, filename, _lineNumber)) return false;
                            }
                        }
                        break;
                            
                        case ThreeBytes:
                        {
                            // BCC
                            if(branch)
                            {
                                // Search for branch label
                                Label label;
                                uint8_t operand = 0x00;
                                if(evaluateLabelOperand(tokens, tokenIndex, label, false))
                                {
                                    operand = uint8_t(label._address) - BRANCH_ADJUSTMENT;
                                }
                                else
                                {
                                    fprintf(stderr, "Assembler::assemble() : Label missing : '%s' : in '%s' on line %d\n", tokens[tokenIndex].c_str(), filename.c_str(), _lineNumber+1);
                                    return false;
                                }

                                instruction._operand0 = branch;
                                instruction._operand1 = operand & 0x00FF;
                                _instructions.push_back(instruction);
                                if(!checkInvalidAddress(ParseType(parse), _currentAddress, outputSize, instruction, lineToken, filename, _lineNumber)) return false;
                            }
                            // All other 3 byte instructions
                            else
                            {
                                uint16_t operand;
                                operandValid = Expression::stringToU16(tokens[tokenIndex], operand);
                                if(!operandValid)
                                {
                                    Label label;
                                    Equate equate;

                                    // Search equates
                                    if(operandValid = evaluateEquateOperand(tokens, tokenIndex, equate, compoundInstruction))
                                    {
                                        operand = equate._operand;
                                    }
                                    // Search labels
                                    else if(operandValid = evaluateLabelOperand(tokens, tokenIndex, label, compoundInstruction))
                                    {
                                        operand = label._address;
                                    }
                                    else if(Expression::isExpression(tokens[tokenIndex]) == Expression::Valid)
                                    {
                                        std::string input;
                                        preProcessExpression(tokens, tokenIndex, input, true);
                                        operand = Expression::parse((char*)input.c_str(), _lineNumber);
                                        operandValid = true;
                                    }
                                    else if(!operandValid)
                                    {
                                        fprintf(stderr, "Assembler::assemble() : Label/Equate error : '%s' : in '%s' on line %d\n", tokens[tokenIndex].c_str(), filename.c_str(), _lineNumber+1);
                                        return false;
                                    }
                                }

                                // Reserved assembler opcode DW, (define word)
                                if(opcodeType == ReservedDW  ||  opcodeType == ReservedDWR)
                                {
                                    // Push first operand
                                    outputSize = TwoBytes;
                                    instruction._isRomAddress = (opcodeType == ReservedDWR) ? true : false;
                                    instruction._byteSize = ByteSize(outputSize);
                                    instruction._opcode   = uint8_t(operand & 0x00FF);
                                    instruction._operand0 = uint8_t((operand & 0xFF00) >>8);
                                    _instructions.push_back(instruction);

                                    // Push any remaining operands
                                    if(tokenIndex + 1 < tokens.size()) handleDefineWord(tokens, tokenIndex, instruction, true, outputSize);
                                    if(!checkInvalidAddress(ParseType(parse), _currentAddress, outputSize, instruction, lineToken, filename, _lineNumber)) return false;
                                }
                                // Normal instructions
                                else
                                {
                                    instruction._operand0 = uint8_t(operand & 0x00FF);
                                    instruction._operand1 = uint8_t((operand & 0xFF00) >>8);
                                    _instructions.push_back(instruction);
                                    if(!checkInvalidAddress(ParseType(parse), _currentAddress, instruction._byteSize, instruction, lineToken, filename, _lineNumber)) return false;
                                }
                            }
                        }
                        break;
                    }
                }

                _currentAddress += outputSize;
            }              
        }

        // Pack byte code buffer from instruction buffer
        packByteCodeBuffer();

        // Parse gprintf labels, equates and expressions
        if(!parseGprintfs()) return false;

        return true;
    }
}
