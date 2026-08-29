file(READ "${MDNS_DISCOVERY_SOURCE}" MDNS_DISCOVERY_TEXT)
file(READ "${STRUCTURED_MDNS_SOURCE}" STRUCTURED_MDNS_TEXT)

string(FIND "${MDNS_DISCOVERY_TEXT}" "parse_mdns_log_message" LOG_PARSER_POSITION)
if(NOT LOG_PARSER_POSITION EQUAL -1)
  message(FATAL_ERROR "mDNS discovery must consume structured records, not parse diagnostic log text")
endif()

string(FIND "${STRUCTURED_MDNS_TEXT}" "mdns_query_recv" PACKET_RECEIVER_POSITION)
if(PACKET_RECEIVER_POSITION EQUAL -1)
  message(FATAL_ERROR "mDNS discovery must decode structured DNS packets")
endif()

string(FIND "${STRUCTURED_MDNS_TEXT}" "Logger::" ADAPTER_LOGGER_POSITION)
if(NOT ADAPTER_LOGGER_POSITION EQUAL -1)
  message(FATAL_ERROR "structured mDNS records must not be sourced from diagnostic logging")
endif()

string(FIND "${MDNS_DISCOVERY_TEXT}" "process_query_record" STRUCTURED_RECORD_POSITION)
if(STRUCTURED_RECORD_POSITION EQUAL -1)
  message(FATAL_ERROR "mDNS discovery is missing its structured record assembler")
endif()

string(FIND "${MDNS_DISCOVERY_TEXT}" "Logger::setLoggerSink" LOGGER_SINK_POSITION)
if(NOT LOGGER_SINK_POSITION EQUAL -1)
  message(FATAL_ERROR "mDNS discovery must not use the global diagnostic logger as a data API")
endif()
