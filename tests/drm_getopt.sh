# This is required for check/distcheck target as it has to --list-subtests
# for each test. Source it if you add a new test in form of a (shell) script.

# See tests/vgem_reload_basic

IGT_EXIT_TIMEOUT=78
IGT_EXIT_SKIP=77
IGT_EXIT_SUCCESS=0
IGT_EXIT_INVALID=79
IGT_EXIT_FAILURE=99

# hacked-up long option parsing
for arg in $@ ; do
	case $arg in
		--list-subtests)
			exit $IGT_EXIT_INVALID
			;;
		--run-subtest)
			exit $IGT_EXIT_INVALID
			;;
		--debug)
			IGT_LOG_LEVEL=debug
			;;
		--help-description)
			echo $IGT_TEST_DESCRIPTION
			exit $IGT_EXIT_SUCCESS
			;;
		--help)
			echo "Usage: `basename $0` [OPTIONS]"
			echo "  --list-subtests"
			echo "  --run-subtest <pattern>"
			echo "  --debug"
			echo "  --help-description"
			echo "  --help"
			exit $IGT_EXIT_SUCCESS
			;;
	esac
done
