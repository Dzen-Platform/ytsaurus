package main

import (
	"bytes"
	"flag"
	"fmt"
	"go/parser"
	"io/ioutil"
	"os"
)

var (
	flagInterface = flag.String("interface", "", "path to interface.go file")
	flagOutput    = flag.String("output", "", "path to output file")
)

func fatalf(msg string, args ...interface{}) {
	line := fmt.Sprintf(msg, args...)
	if line[len(line)-1] != '\n' {
		line += "\n"
	}

	_, _ = os.Stderr.WriteString(line)
	os.Exit(2)
}

func main() {
	flag.Parse()
	if *flagInterface == "" || *flagOutput == "" {
		flag.Usage()
		os.Exit(2)
	}

	node, err := parser.ParseFile(fset, *flagInterface, nil, parser.ParseComments)
	if err != nil {
		fatalf("%v", err)
	}

	file, err := parseFile(node)
	if err != nil {
		fatalf("%v", err)
	}

	var buf bytes.Buffer
	if err = emit(file, &buf); err != nil {
		fatalf("%v", err)
	}

	if err = ioutil.WriteFile(*flagOutput, buf.Bytes(), 0644); err != nil {
		fatalf("%v", err)
	}
}
