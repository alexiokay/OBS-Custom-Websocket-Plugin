file(READ "${OBS_PLUGIN_SOURCE}" OBS_PLUGIN_TEXT)
file(READ "${OVERLAY_SOURCE}" OVERLAY_SOURCE_TEXT)

function(require_text CONTENT NEEDLE DESCRIPTION)
  string(FIND "${CONTENT}" "${NEEDLE}" POSITION)
  if(POSITION EQUAL -1)
    message(FATAL_ERROR "Overlay capability policy failed: ${DESCRIPTION}")
  endif()
endfunction()

function(forbid_text CONTENT NEEDLE DESCRIPTION)
  string(FIND "${CONTENT}" "${NEEDLE}" POSITION)
  if(NOT POSITION EQUAL -1)
    message(FATAL_ERROR "Overlay capability policy failed: ${DESCRIPTION}")
  endif()
endfunction()

require_text(
  "${OBS_PLUGIN_TEXT}"
  "set_global_overlay_capability_url(url);"
  "host-issued overlay capabilities must be retained for future sources")
require_text(
  "${OVERLAY_SOURCE_TEXT}"
  "default_url = \"about:blank\";"
  "an overlay without a host-issued capability must remain inert")
forbid_text(
  "${OBS_PLUGIN_TEXT}"
  "update_all_overlay_urls_to_connected_server"
  "mDNS/WebSocket discovery must not reconstruct tokenless overlay URLs")
forbid_text(
  "${OBS_PLUGIN_TEXT}"
  "Received payload:"
  "protocol payloads can contain credentials and must not be logged")
forbid_text(
  "${OVERLAY_SOURCE_TEXT}"
  "URL changed from"
  "browser-source capability values must not be written to OBS logs")
