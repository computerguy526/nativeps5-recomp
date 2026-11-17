// selfunwrap writes the plain ELF inside a PS4/PS5 (fake-signed) SELF: selfunwrap <in.self> <out.elf>
package main

import (
	"fmt"
	"os"

	"ps4-recomp/pkg/elfloader"
)

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: selfunwrap <in> <out.elf>")
		os.Exit(2)
	}
	data, err := os.ReadFile(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	if elfloader.IsSELF(data) {
		if data, err = elfloader.ExtractELFFromSELF(data); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	}
	if err := os.WriteFile(os.Args[2], data, 0o644); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
