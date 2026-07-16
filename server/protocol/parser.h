/*
 * ============================================================================
 * Event Ticketing Platform — Protocol Parser (Header)
 * ============================================================================
 * Parses raw text commands from clients into structured parsed_command_t.
 * Tokenizes input by whitespace, identifies the command type, and extracts
 * arguments.
 * ============================================================================
 */

#ifndef ETP_PARSER_H
#define ETP_PARSER_H

#include "../../common/protocol.h"

/*
 * Parse a raw command string into a parsed_command_t.
 *
 * Input:  "BOOK 1 A1 A2 A3"
 * Output: { type=CMD_BOOK, argc=4, args=["1","A1","A2","A3"] }
 *
 * Returns 0 on success, -1 if input is empty or invalid.
 */
int parse_command(const char *raw, parsed_command_t *cmd);

/*
 * Format a response string.
 *
 * OK responses:    "OK <payload>\n"
 * Error responses: "ERROR <message>\n"
 * Denied:          "DENIED <reason>\n"
 */
int format_response(char *buf, size_t buf_size, response_status_t status,
                    const char *fmt, ...);

#endif /* ETP_PARSER_H */
