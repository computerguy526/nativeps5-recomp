package uecvar

import (
	"strings"
	"testing"
)

func utf16(s string) []byte {
	var b []byte
	for _, c := range []byte(s) {
		b = append(b, c, 0)
	}
	return append(b, 0, 0)
}

func TestScanAndProfile(t *testing.T) {
	data := append([]byte("xx\x01\x01"), utf16("r.Nanite")...)
	data = append(data, utf16("r.RayTracing.Shadows")...)
	data = append(data, []byte("t.MaxFPS\x00hello.txt\x00Foo.Bar\x00")...)
	names := Scan(data)
	if strings.Join(names, ",") != "r.Nanite,r.RayTracing.Shadows,t.MaxFPS" {
		t.Fatalf("names %v", names)
	}
	p, err := Parse(strings.NewReader("# c\nr.Nanite=0\nr.Bogus=1 # x\n[/Script/Engine.RendererSettings]\nr.RayTracing=0\n@cmdline -nosound\n"))
	if err != nil || len(p.Entries) != 3 {
		t.Fatal(err, p)
	}
	u := p.Unknown(names)
	if len(u) != 1 || u[0].Name != "r.Bogus" {
		t.Fatalf("unknown %v", u)
	}
	ini := p.EngineINI()
	if !strings.Contains(ini, "[SystemSettings]\nr.Nanite=0\nr.Bogus=1\n") || !strings.Contains(ini, "[/Script/Engine.RendererSettings]\nr.RayTracing=0\n") {
		t.Fatal(ini)
	}
	if got := p.CommandLine("-project=x\n"); got != "-project=x -nosound\n" {
		t.Fatal(got)
	}
	if _, err := Parse(strings.NewReader("novalue\n")); err == nil {
		t.Fatal("want error")
	}
}
