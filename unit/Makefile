# -*- Mode: makefile-gmake -*-

.PHONY: clean all

all:
%:
	@$(MAKE) -C unit_base $*
	@$(MAKE) -C unit_sim_settings $*

clean: unitclean
	rm -f coverage/*.gcov
	rm -fr coverage/report
