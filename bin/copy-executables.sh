#!/usr/bin/env bash
HOSTS=( "houston" "sanantonio")
HOSTS=( "houston" "sanantonio" "austin" )
#HOSTS=( "austin" "houston" "sanantonio" "indianapolis" "philly" )
#HOSTS=( "austin" "houston" "sanantonio" "indianapolis" "philly" "baltimore" "chicago" "atlanta" "detroit")
LOCAL_HOST=`hostname`
#EXECUTABLES=( "hermes" "run-hermes.sh" )
EXECUTABLES=( "hermes" )
MAKE_FOLDER="/home/user/hermes/src"
HOME_FOLDER="/home/user/hermes/src/hermes"
DEST_FOLDER="/home/user/hermes-exec/src/hermes"

cd $MAKE_FOLDER
make clean
make
cd -

for EXEC in "${EXECUTABLES[@]}"
do
	#echo "${EXEC} copied to {${HOSTS[@]/$LOCAL_HOST}}"
	parallel scp ${HOME_FOLDER}/${EXEC} {}:${DEST_FOLDER}/${EXEC} ::: $(echo ${HOSTS[@]/$LOCAL_HOST})
	echo "${EXEC} copied to {${HOSTS[@]/$LOCAL_HOST}}"
done
