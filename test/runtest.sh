#!/bin/sh

# run test images through libnsgif and count results

TEST_PATH=$1
TEST_OUT=${TEST_PATH}/ppm
TEST_LOG=${TEST_PATH}/test.log

mkdir -p ${TEST_OUT}

echo "GIF tests" > ${TEST_LOG}

# gif test directories

# netsurf test gif
GIFTESTS="test/data/*.gif"

# netsurf afl generated gif suite
GIFTESTS="${GIFTESTS} test/ns-afl-gif/*.gif"

# Print the virtual memory limit (KiB) a test runs with, if any.
gifulimit()
{
	METAF=$(dirname ${1})/$(basename ${1} .gif).meta
	if [ -f "${METAF}" ]; then
		sed -n 's/^ulimit: \{0,1\}//p' ${METAF}
	fi
}

# Hacky detection of whether built with sanitizer.
SANITIZED=no
if grep -qaE '__(a|hwa|m|t)san_init' ${TEST_PATH}/test_nsgif; then
	SANITIZED=yes
fi

# Check if the test needs to be skipped.
gifskip()
{
	ULIMIT=$(gifulimit ${1})
	if [ -z "${ULIMIT}" ]; then
		return 1
	fi
	if [ "${SANITIZED}" = "yes" ]; then
		echo "GIF:${1}" >> ${TEST_LOG}
		echo "Skipped: memory limited test in sanitizer build" >> ${TEST_LOG}
		return 0
	fi
	if ! (ulimit -v ${ULIMIT}) 2> /dev/null; then
		echo "GIF:${1}" >> ${TEST_LOG}
		echo "Skipped: ulimit -v unsupported" >> ${TEST_LOG}
		return 0
	fi
	return 1
}

gifdecode()
{
	OUTF=$(basename ${1} .gif)
	CMPF=$(dirname  ${1})/${OUTF}
	METAF=$(dirname  ${1})/${OUTF}.meta
	ULIMIT=$(gifulimit ${1})
	echo "GIF:${1}" >> ${TEST_LOG}
	if [ -n "${ULIMIT}" ]; then
		(ulimit -v ${ULIMIT} && exec ${TEST_PATH}/test_nsgif ${1}) 2> ${TEST_OUT}/${OUTF}.err
	else
		${TEST_PATH}/test_nsgif ${1} --ppm ${TEST_OUT}/${OUTF}.ppm --pbm ${TEST_OUT}/${OUTF}.pbm 2> ${TEST_OUT}/${OUTF}.err
	fi
	ECODE=$?
	cat ${TEST_OUT}/${OUTF}.err >> ${TEST_LOG}

	echo "Exit code:${ECODE}" >> ${TEST_LOG}
	if [ "${ECODE}" -gt 0 ];then
		return ${ECODE}
	fi

	# The PPM has the colour channels and the PBM has the transparency mask.
	for EXT in ppm pbm; do
		if [ -f "${CMPF}.${EXT}" ]; then
			cmp ${CMPF}.${EXT} ${TEST_OUT}/${OUTF}.${EXT} >> ${TEST_LOG} 2>> ${TEST_LOG}
			if [ "$?" -ne 0 ]; then
				return 128
			fi
		fi
	done

	if [ -f "${METAF}" ]; then
		# The "stderr:" lines in the metadata are the expected stderr.
		sed -n 's/^stderr: \{0,1\}//p' ${METAF} | cmp - ${TEST_OUT}/${OUTF}.err >> ${TEST_LOG} 2>> ${TEST_LOG}
		if [ "$?" -ne 0 ]; then
			return 128
		fi
	fi

	return 0
}

GIFTESTTOTC=0
GIFTESTPASSC=0
GIFTESTFAILC=0
GIFTESTERRC=0
GIFTESTSKIPC=0

echo "Testing GIF decode"

for GIF in $(ls ${GIFTESTS});do
	GIFTESTTOTC=$((GIFTESTTOTC+1))
	#echo "${GIF}"
	if gifskip ${GIF}; then
		GIFTESTSKIPC=$((GIFTESTSKIPC+1))
		continue
	fi
	gifdecode ${GIF}
	ECODE=$?
	if [ "${ECODE}" -gt 127 ];then
		GIFTESTERRC=$((GIFTESTERRC+1))
		echo "Error ${GIF}"
	else
		if [ "${ECODE}" -gt 0 ];then
			GIFTESTFAILC=$((GIFTESTFAILC+1))
		else
			GIFTESTPASSC=$((GIFTESTPASSC+1))
		fi
	fi
done

echo "Tests:${GIFTESTTOTC} Pass:${GIFTESTPASSC} Fail:${GIFTESTFAILC} Error:${GIFTESTERRC} Skip:${GIFTESTSKIPC}"

# exit code
if [ "${GIFTESTERRC}" -gt 0 ]; then
	exit 1
fi

exit 0
