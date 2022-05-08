# -*- Mode: makefile-gmake -*-

.PHONY: clean all

all:
%:
	@$(MAKE) -C unit_base $*
	@$(MAKE) -C unit_ext_ims $*
	@$(MAKE) -C unit_ext_plugin $*
	@$(MAKE) -C unit_ext_slot $*
	@$(MAKE) -C unit_sim_settings $*

clean: unitclean
	rm -f coverage/*.gcov
	rm -fr coverage/report
