# Shared presentation policy. Errors, compiler diagnostics and test output are
# never redirected; VERBOSE=1 restores the complete recipes.
VERBOSE ?= 0
export VERBOSE
ifeq ($(VERBOSE),1)
progress =
else
.SILENT:
MAKEFLAGS += --no-print-directory
progress = @printf '%s\n' '$(1)'
endif
# Failed commands must not leave apparently up-to-date binaries/headers.
.DELETE_ON_ERROR:
