/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/types/pcre_regex.c
 *
 * PCRE2 wrappers (Perl Compatible Regular Expression)
 *
 *-------------------------------------------------------------------------
 */

#define PCRE2_CODE_UNIT_WIDTH 8
#define REGEX_MAX_PATTERN_LENGTH 32761
#define REGEX_MAX_PATTERN_LENGTH_AGGREGATION 32764
#define PCRE2_RECURSION_LIMIT 4001
#define MIN_JIT_STACK_SIZE (32 * 1024)
#define MAX_JIT_STACK_SIZE (MIN_JIT_STACK_SIZE * 2)

#include <pcre2.h>
#include <postgres.h>
#include <utils/builtins.h>
#include <lib/stringinfo.h>
#include "io/bson_core.h"
#include "utils/documentdb_errors.h"
#include "types/pcre_regex.h"

/* Data needed during the PCRE2 lib usage for regex compile and match */
typedef struct PcreData
{
	/* Compile Context for regex compilation */
	pcre2_compile_context *compileContext;

	/* Context for Custom memory management */
	pcre2_general_context *generalContext;

	/* Context used while pattern matching */
	pcre2_match_context *matchContext;

	/* Match data block for holding the result of a regex match */
	pcre2_match_data *matchData;

	/* compiled regex data generated by pcre2_compile function will be used during pcre2_match */
	pcre2_code *compiledRegex;

	/* stack for use by the code compiled by the JIT compiler */
	pcre2_jit_stack *jitStack;
} PcreData;

/* --------------------------------------------------------- */
/* Forward declaration */
/* --------------------------------------------------------- */

static uint32_t ProcessRegexCompileOptions(char *options);
static inline void CreatePcreCompileContext(PcreData *pcreData);
static inline void InvalidRegexError(int errorCode, const char *errorMessage, int
									 pcreErrorCode, PcreData *pcreData);
static bool RegexCompileCore(char *regexPatternStr, char *options, PcreData **pcreData,
							 int *pcreErrorCode, int maxPatternLength,
							 uint32_t compileOptions);
void * extension_pcre_malloc(PCRE2_SIZE size, void *ignore);
void extension_pcre_free(void *memPtr, void *ignore);

/* This function is used to get registered with PCRE2 context
 * during the calls to PCRE2 functions so that PCRE2 uses
 * this function to allocate the memory. As palloc gets
 * memory from Postgres, memory management will be taken
 * care by Postgres. */
void *
extension_pcre_malloc(PCRE2_SIZE size, void *ignore)
{
	return palloc((size_t) size);
}


/* This function is used to get registered with PCRE2 context
 * during the calls to PCRE2 functions so that PCRE2 uses
 * this function to free the memory. pfree frees
 * memory to Postgres context */
void
extension_pcre_free(void *memPtr, void *ignore)
{
	if (memPtr != NULL)
	{
		pfree(memPtr);
	}
}


/* Top level function to call RegexCompileCore to validate
 * regex pattern string in planning phase */
void
RegexCompileDuringPlanning(char *regexPatternStr, char *options)
{
	PcreData *pcreData = palloc0(sizeof(PcreData));
	int pcreErrorCode = 0;

	if (!RegexCompileCore(regexPatternStr, options, &pcreData, &pcreErrorCode,
						  REGEX_MAX_PATTERN_LENGTH, PCRE2_NO_AUTO_CAPTURE))
	{
		InvalidRegexError(ERRCODE_DOCUMENTDB_LOCATION51091,
						  "Regular expression is invalid",
						  pcreErrorCode, pcreData);
	}
	pcre2_compile_context_free(pcreData->compileContext);
	pcre2_general_context_free(pcreData->generalContext);
}


/* Top level function to call RegexCompileCore at execution phase */
PcreData *
RegexCompile(char *regexPatternStr, char *options)
{
	PcreData *pcreData = palloc0(sizeof(PcreData));
	int pcreErrorCode = 0;

	if (!RegexCompileCore(regexPatternStr, options, &pcreData, &pcreErrorCode,
						  REGEX_MAX_PATTERN_LENGTH, PCRE2_NO_AUTO_CAPTURE))
	{
		InvalidRegexError(ERRCODE_DOCUMENTDB_LOCATION51091,
						  "Regular expression is invalid",
						  pcreErrorCode, pcreData);
	}

	/* Creates a new matchData block to hold the result of a match */
	pcreData->matchData =
		pcre2_match_data_create_from_pattern(pcreData->compiledRegex, NULL);
	return pcreData;
}


/* Top level function to call RegexCompileCore for aggregation opeartors */
PcreData *
RegexCompileForAggregation(char *regexPatternStr, char *options, bool enableNoAutoCapture,
						   const char *regexInvalidErrorMessage)
{
	PcreData *pcreData = palloc0(sizeof(PcreData));

	int pcreErrorCode = 0;

	uint32_t compileOptions = enableNoAutoCapture ? PCRE2_NO_AUTO_CAPTURE : 0;
	if (!RegexCompileCore(regexPatternStr, options, &pcreData, &pcreErrorCode,
						  REGEX_MAX_PATTERN_LENGTH_AGGREGATION, compileOptions))
	{
		InvalidRegexError(ERRCODE_DOCUMENTDB_LOCATION51111, regexInvalidErrorMessage,
						  pcreErrorCode,
						  pcreData);
	}

	/* we pass pcre2_general_context to the input so for memory allocation we use custom function of general context : palloc and pfree */
	pcreData->matchContext = pcre2_match_context_create(pcreData->generalContext);
	pcre2_set_recursion_limit(pcreData->matchContext, PCRE2_RECURSION_LIMIT);

	/* create a stack for use by the code compiled by the JIT compiler */
	pcreData->jitStack = pcre2_jit_stack_create(MIN_JIT_STACK_SIZE, MAX_JIT_STACK_SIZE,
												pcreData->generalContext);
	if (pcreData->jitStack == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_EXCEEDEDMEMORYLIMIT), errmsg(
							"PCRE2 stack creation failure.")));
	}

	/* provides control over the memory used by JIT as a run-time stack */
	pcre2_jit_stack_assign(pcreData->matchContext, NULL, pcreData->jitStack);

	/* Creates a new matchData block to hold the result of a match */
	pcreData->matchData =
		pcre2_match_data_create_from_pattern(pcreData->compiledRegex, NULL);
	return pcreData;
}


/*
 * allocate memory for the input pointer 'pcreData' and then populate it with the compiled 'pcredata' after compiling the regex expression.
 * If the regex compilation is successful, the function will return true; otherwise, it will return false, while also populating the error code into the input variable 'pcreErrorCode'
 */
static bool
RegexCompileCore(char *regexPatternStr, char *options, PcreData **pcreData,
				 int *pcreErrorCode, int maxPatternLength, uint32_t compileOptions)
{
	PCRE2_SIZE errorOffset;

	/* PCRE2_SPTR is a pointer to unsigned code units of the appropriate width
	 * (in this case, 8 bits).*/
	PCRE2_SPTR pattern = (PCRE2_SPTR) regexPatternStr;

	compileOptions |= ProcessRegexCompileOptions(options);

	/* Creates PCRE2 general and compile contexts. This will be needed to
	 * register the PG's memory management functions with PCRE2 lib */
	CreatePcreCompileContext(*pcreData);

	pcre2_set_max_pattern_length((*pcreData)->compileContext, maxPatternLength);

	(*pcreData)->compiledRegex =
		pcre2_compile(pattern,                   /* the pattern */
					  PCRE2_ZERO_TERMINATED,     /* indicates pattern is zero-terminated */
					  compileOptions,            /* RE Compile options */
					  pcreErrorCode,              /* for error number */
					  &errorOffset,              /* for error offset */
					  (*pcreData)->compileContext);

	if ((*pcreData)->compiledRegex != NULL)
	{
		if (pcre2_jit_compile((*pcreData)->compiledRegex, PCRE2_JIT_COMPLETE) ==
			PCRE2_ERROR_NOMEMORY)
		{
			ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_EXCEEDEDMEMORYLIMIT),
							errmsg(
								"There isn't enough available memory to perform the evaluation of the regular expression.")));
		}
		return true;
	}

	return false;
}


/*
 * Execute the PCRE regular expression provided in regexData against
 * the source string (which is subjectString)
 */
bool
PcreRegexExecute(char *regexPatternStr, char *options,
				 PcreData *pcreData,
				 const StringView *subjectString)
{
	bool matched = true;

	Assert(pcreData != NULL);

	/* Now run the match. */
	int returnCode = pcre2_match(pcreData->compiledRegex,
								 (PCRE2_SPTR) subjectString->string,
								 (PCRE2_SIZE) subjectString->length,
								 0, 0, pcreData->matchData, pcreData->matchContext);

	if (returnCode == PCRE2_ERROR_RECURSIONLIMIT)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_LOCATION51156), errmsg(
							"Error occurred while executing the regular expression. Result code: -21")));
	}

	if (returnCode < 0)
	{
		matched = false;
	}

	return matched;
}


/*
 * Retrieves the output vector using the provided PCRE data and returns it.
 * The output vector is an array of size_t with the following structure:
 * output[0]: Start index of the match in the input string.
 * output[1]: End index of the match in the input string.
 *
 * For the remaining elements in the output vector:
 * Even-indexed elements represent the start index of i th capture group in the input string.
 * Odd-indexed elements represent the end index of i th capture group in the input string.
 * To determine the size of the output vector, you can use the GetResultLengthUsingPcreData function.
 */
size_t *
GetResultVectorUsingPcreData(PcreData *pcreData)
{
	return pcre2_get_ovector_pointer(pcreData->matchData);
}


/*
 * Retrieves the length of the output vector using the provided PCRE data and returns it.
 */
int
GetResultLengthUsingPcreData(PcreData *pcreData)
{
	return pcre2_get_ovector_count(pcreData->matchData);
}


/*
 * Checks if the provided regex options are valid.
 */
bool
IsValidRegexOptions(char *options)
{
	if (options != NULL)
	{
		for (int i = 0; options[i] != '\0'; i++)
		{
			switch (options[i])
			{
				case 'i':
				case 's':
				case 'm':
				case 'x':
				case 'u':
				{
					continue;
				}

				default:
				{
					return false;
				}
			}
		}
	}

	return true;
}


/* --------------------------------------------------------- */
/* Private helper methods */
/* --------------------------------------------------------- */


/*
 * Process the Regular expression compile options
 */
static uint32_t
ProcessRegexCompileOptions(char *options)
{
	uint32_t compileOptions = PCRE2_UTF;

	/* Handle regex options, if provided, by setting up the appropriate
	 * regex compile flags */
	if (options != NULL)
	{
		for (int i = 0; options[i] != '\0'; i++)
		{
			switch (options[i])
			{
				case 'i':
				{
					compileOptions |= PCRE2_CASELESS;
					break;
				}

				case 's':
				{
					/* Allows the dot character (i.e. .) to match all
					 * characters including newline characters */
					compileOptions |= PCRE2_DOTALL;
					break;
				}

				case 'm':
				{
					/* Multi line support */
					compileOptions |= PCRE2_MULTILINE;
					break;
				}

				case 'x':
				{
					/* Expanded Mode flag where pattern contains
					 * whitespace, comments */
					compileOptions |= PCRE2_EXTENDED;
					break;
				}

				default:
				{
					/* Unknown options to be ignored */
				}
			}
		}
	}

	return compileOptions;
}


/* function to free all internal data members of the PcreData struct */
void
FreePcreData(PcreData *pcreData)
{
	if (!pcreData)
	{
		return;
	}

	/* below all functions : If the argument is NULL, the function returns immediately without doing anything. */
	pcre2_compile_context_free(pcreData->compileContext);
	pcre2_general_context_free(pcreData->generalContext);
	pcre2_match_context_free(pcreData->matchContext);
	pcre2_match_data_free(pcreData->matchData);
	pcre2_code_free(pcreData->compiledRegex);
	pcre2_jit_stack_free(pcreData->jitStack);
}


/*
 * Create Compile Context for PCRE2 matching function
 */
static inline void
CreatePcreCompileContext(PcreData *pcreData)
{
	pcreData->generalContext = pcre2_general_context_create(extension_pcre_malloc,
															extension_pcre_free,
															NULL);
	if (pcreData->generalContext == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_EXCEEDEDMEMORYLIMIT), errmsg(
							"PCRE2 general context creation failure.")));
	}

	pcreData->compileContext = pcre2_compile_context_create(pcreData->generalContext);
	if (pcreData->compileContext == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_DOCUMENTDB_EXCEEDEDMEMORYLIMIT), errmsg(
							"PCRE2 compile context creation failure.")));
	}
}


/*
 * helper function to throw invalid regex pattern error with different error-codes and error-messages.
 * after throwing the error function free the pcre contexts.
 */
static inline void
InvalidRegexError(int errorCode, const char *errorMessage, int pcreErrorCode,
				  PcreData *pcreData)
{
	PCRE2_UCHAR buffer[256];
	pcre2_get_error_message(pcreErrorCode, buffer, sizeof(buffer));
	FreePcreData(pcreData);
	ereport(ERROR, (errcode(errorCode),
					errmsg("%s: %s", errorMessage, buffer),
					errdetail_log("PCRE returned invalid regex: error code %d",
								  pcreErrorCode)));
}
