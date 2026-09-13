# Patch the generated Emscripten JS for browser-specific canvas behavior.
function(patch_webgpu_js html_file)
    if(NOT EXISTS "${html_file}")
        message(WARNING "patch_webgpu_js: file not found: ${html_file}")
        return()
    endif()
    file(READ "${html_file}" _html)
    set(_patches_applied 0)

    set(_scissor_call "assert(g>=0),assert(I>=0),assert(B>=0),assert(Q>=0),WebGPU.getJsObject(A).setScissorRect(g,I,B,Q)")
    string(FIND "${_html}" "${_scissor_call}" _scissor_pos)
    if(_scissor_pos GREATER -1)
        string(REPLACE "${_scissor_call}"
            "assert(g>=0),assert(I>=0),assert(B>=0),assert(Q>=0),WebGPU.getJsObject(A).setScissorRect(g,I,Math.min(B,(document.getElementById('gladius-canvas')?.width||B)-g),Math.min(Q,(document.getElementById('gladius-canvas')?.height||Q)-I))"
            _html "${_html}")
        math(EXPR _patches_applied "${_patches_applied}+1")
    endif()

    if(_patches_applied GREATER 0)
        file(WRITE "${html_file}" "${_html}")
        message(STATUS "patch_webgpu_js: applied ${_patches_applied} patch(es) to ${html_file}")
    else()
        message(STATUS "patch_webgpu_js: no patch needed for ${html_file}")
    endif()
endfunction()

# Allow the script to be run standalone with
#   cmake -DPATCH_FUNCTION=patch_webgpu_js -DHTML_FILE=... -P patch_webgpu_js.cmake
if(DEFINED HTML_FILE AND DEFINED PATCH_FUNCTION)
    patch_webgpu_js("${HTML_FILE}")
endif()
