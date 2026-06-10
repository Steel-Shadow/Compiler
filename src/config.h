//
// Created by Steel_Shadow on 2023/10/15.
//

#ifndef COMPILER_CONFIG_H
#define COMPILER_CONFIG_H

#define FILEOUT_ERROR
// #define FILEOUT_LEXER
// #define FILEOUT_PARSER
#define FILEOUT_IR
#define FILEOUT_MIPS


#if defined(MY_DEBUG)
// #define STDOUT_LEXER
// #define STDOUT_PARSER
#define STDOUT_ERROR
// #define STDOUT_IR
// #define STDOUT_MIPS
#endif


#endif
