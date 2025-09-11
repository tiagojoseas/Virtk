#!/bin/bash

# if $1 -m: make
# if $1 -c: make clean 
# if $1 -i: insert module
# if $1 -r: remove module
# if $1 -h: help

make_module() {
    make
}

clean_module() {
    make clean > /dev/null
}

insert_module() {
    make_module
    insmod iw.ko
    iw dev wlp0s20f3 link > iw.log
    dmesg | tail -n 40
    clean_module
}

remove_module() {
    rmmod iw --force
}


case "$1" in
    -m|--make)
        make_module
        ;;
    -c|--clean)
        clean_module
        ;;
    -i|--insert-module)
        insert_module
        ;;
    -r|--remove-module)
        remove_module
        ;;
    -ri|--remove-insert-module)
        remove_module
        insert_module
        ;;
    -h|--help)
        echo "Usage: $0 [option]"
        echo "Options:"
        echo "  -m, --make: make"
        echo "  -c, --clean: make clean"
        echo "  -i, --insert-module: insert module"
        echo "  -r, --remove-module: remove module"
        echo "  -ri, --remove-insert-module: remove and insert module"
        echo "  -h, --help: help"
        ;;
    *)
        echo "Invalid option: $1"
        ;;
esac