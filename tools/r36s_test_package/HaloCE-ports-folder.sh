#!/bin/bash
# Put this HaloCE.sh in your ports folder NEXT TO the halo-r36s-test folder.
# It just runs the launcher inside the folder (all logging happens there).
cd "$(dirname "$0")/halo-r36s-test" && bash ./HaloCE.sh
