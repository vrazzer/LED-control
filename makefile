all: spe6ctrl

spe6ctrl: spe6ctrl.c
	gcc -g -O0 $@.c -o $@ -lbluetooth

clean:
	rm -f spe6ctrl

