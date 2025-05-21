.PHONY: test
test:
	npm run tsc ; npm run test

.PHONY:	compile
compile:
	npm run tsc

.PHONY:	format
format:
	find . -type f \
		\( -wholename "./src/*" -or -wholename "./tests/*" \) -and \
		\( -iname "*.cpp" -or -iname "*.cpp" -or -iname "*.js" -or \
		-iname "*.ts" \) -exec clang-format -i "{}" \;

.PHONY: docs
docs:
	doxygen
	@make -C latex > /dev/null
	mv latex/refman.pdf .

################################################################
# Containerization stuff
################################################################

# For absolute path usage later
cwd := $(shell pwd)

.PHONY:	docker
docker:
	docker build --tag 'jsonx' .
	docker run \
		--mount type=bind,source="${cwd}",target="/home/user/host" \
		-i \
		-t jsonx:latest \

.PHONY:	podman
podman:
	podman build --tag 'jsonx' .
	podman run \
		--mount type=bind,source="${cwd}",target="/host" \
		--mount type=bind,source="/",target="/hostroot" \
		-i \
		-t jsonx:latest \
