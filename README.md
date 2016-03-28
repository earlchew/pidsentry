Multiboot Chain Loader

This is a multiboot module that conforms to the Multiboot Specification
version 0.6.96:

    http://www.gnu.org/software/grub/manual/multiboot/multiboot.html

The module can be loaded by GRUB 2 and used to chainload subsequent
boot loaders.

The functionality of this module differs from the GRUB chainloader command
in the following key ways:

    The GRUB chainloader command loads a single sector whereas this module
    loads an entire file.

    The GRUB 2 chainloader command passes the following parameters to the
    target whereas this module always passes these parameters as zero:

        %esi  Boot partition
        %edx  Boot drive

Both the GRUB chainloader and this module load the target code at 0x0:0x7c00
and executes the code in x86 real mode.

The main purpose of the module is to allow GRUB 2 to chainload into legacy
GRUB or other boot loader implementations that must execute in the first 1MB
of memory.
