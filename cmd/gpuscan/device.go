package main

import (
	"crypto/sha1"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
)

// Device holds the capabilities of one Vulkan device as reported by `vulkaninfo --json`.
type Device struct {
	Source     string                `json:"source"` // how the data was obtained
	Name       string                `json:"name"`
	Driver     string                `json:"driver"`
	APIVersion string                `json:"apiVersion"`
	Features   map[string]bool       `json:"features"`
	Limits     map[string]float64    `json:"limits"`
	Props      map[string]string     `json:"properties,omitempty"`
	Extensions []string              `json:"extensions"`
	formats    map[string]formatCaps // not serialised: large
	ext        map[string]bool
	Lists      map[string][]string `json:"-"`
	SubgroupOp map[string]bool     `json:"-"`
	SubgroupSt map[string]bool     `json:"-"`
	Cached     bool                `json:"cached,omitempty"`
	Queues     int                 `json:"computeCapableQueues"`
}

type formatCaps struct{ optimal, linear, buffer map[string]bool }

func (d *Device) Feat(n string) bool { return d != nil && d.Features[n] }
func (d *Device) HasExt(n string) bool {
	return d != nil && d.ext[n]
}
func (d *Device) Limit(n string) (float64, bool) {
	if d == nil {
		return 0, false
	}
	v, ok := d.Limits[n]
	return v, ok
}

// FormatHas reports whether optimal-tiling feature bit (e.g. COLOR_ATTACHMENT) exists for the format name.
func (d *Device) FormatHas(format, bit string) (has, known bool) {
	if d == nil || d.formats == nil {
		return false, false
	}
	fc, ok := d.formats[format]
	if !ok {
		return false, true // vulkaninfo lists every supported format; a missing one is unsupported
	}
	return fc.optimal["VK_FORMAT_FEATURE_"+bit+"_BIT"], true
}

func strList(v any) []string {
	var out []string
	if l, ok := v.([]any); ok {
		for _, x := range l {
			if s, ok := x.(string); ok {
				out = append(out, s)
			}
		}
	}
	return out
}

// ParseVulkanInfo converts the JSON that `vulkaninfo --json` writes into a Device.
func ParseVulkanInfo(data []byte) (*Device, error) {
	var root struct {
		Capabilities map[string]struct {
			Extensions map[string]any              `json:"extensions"`
			Features   map[string]map[string]any   `json:"features"`
			Properties map[string]map[string]any   `json:"properties"`
			Queues     []map[string]map[string]any `json:"queueFamiliesProperties"`
			Formats    map[string]map[string]struct {
				Linear  []string `json:"linearTilingFeatures"`
				Optimal []string `json:"optimalTilingFeatures"`
				Buffer  []string `json:"bufferFeatures"`
			} `json:"formats"`
		} `json:"capabilities"`
	}
	if err := json.Unmarshal(data, &root); err != nil {
		return nil, err
	}
	var dev *Device
	for _, c := range root.Capabilities {
		d := &Device{Features: map[string]bool{}, Limits: map[string]float64{}, Props: map[string]string{}, formats: map[string]formatCaps{}, ext: map[string]bool{}, Lists: map[string][]string{}, SubgroupOp: map[string]bool{}, SubgroupSt: map[string]bool{}}
		for e := range c.Extensions {
			d.ext[e] = true
			d.Extensions = append(d.Extensions, e)
		}
		sort.Strings(d.Extensions)
		for _, st := range c.Features {
			for k, v := range st {
				if b, ok := v.(bool); ok {
					d.Features[k] = d.Features[k] || b
				}
			}
		}
		for sname, st := range c.Properties {
			for k, v := range st {
				switch x := v.(type) {
				case float64:
					d.Limits[k] = x
				case bool:
					if x {
						d.Limits[k] = 1
					} else {
						d.Limits[k] = 0
					}
					d.Props[k] = fmt.Sprint(x)
				case string:
					d.Props[k] = x
				case map[string]any:
					if k == "limits" {
						for lk, lv := range x {
							if f, ok := lv.(float64); ok {
								d.Limits[lk] = f
							}
							if l := strList(lv); len(l) > 0 {
								d.Lists[lk] = append(d.Lists[lk], l...)
							}
						}
					}
				case []any:
					if l := strList(x); len(l) > 0 {
						d.Lists[k] = append(d.Lists[k], l...)
					}
					if k == "subgroupSupportedOperations" {
						for _, s := range strList(x) {
							d.SubgroupOp[s] = true
						}
					}
					if k == "subgroupSupportedStages" {
						for _, s := range strList(x) {
							d.SubgroupSt[s] = true
						}
					}
				}
			}
			_ = sname
		}
		if p, ok := c.Properties["VkPhysicalDeviceProperties"]; ok {
			d.Name, _ = p["deviceName"].(string)
			if v, ok := p["apiVersion"].(float64); ok {
				u := uint32(v)
				d.APIVersion = fmt.Sprintf("%d.%d.%d", u>>22, (u>>12)&0x3ff, u&0xfff)
			}
		}
		if p, ok := c.Properties["VkPhysicalDeviceVulkan12Properties"]; ok {
			n, _ := p["driverName"].(string)
			i, _ := p["driverInfo"].(string)
			d.Driver = strings.TrimSpace(n + " " + i)
		}
		for _, group := range c.Formats {
			_ = group
		}
		for name, fm := range c.Formats {
			fc := formatCaps{map[string]bool{}, map[string]bool{}, map[string]bool{}}
			for _, p := range fm {
				for _, x := range p.Optimal {
					fc.optimal[x] = true
				}
				for _, x := range p.Linear {
					fc.linear[x] = true
				}
				for _, x := range p.Buffer {
					fc.buffer[x] = true
				}
			}
			d.formats[name] = fc
		}
		for _, q := range c.Queues {
			if p, ok := q["VkQueueFamilyProperties"]; ok {
				fl := strList(p["queueFlags"])
				for _, f := range fl {
					if f == "VK_QUEUE_COMPUTE_BIT" {
						if n, ok := p["queueCount"].(float64); ok {
							d.Queues += int(n)
						}
					}
				}
			}
		}
		dev = d
		break
	}
	if dev == nil {
		return nil, fmt.Errorf("no capabilities.device in vulkaninfo JSON")
	}
	return dev, nil
}

// LoadDevice resolves arg (a vulkaninfo JSON file, an ICD manifest, an install dir containing
// share/vulkan/icd.d/*.json, or "" for the default loader) to a Device. ICD runs are cached by
// manifest+library identity in the user cache directory.
func LoadDevice(arg string, noCache bool) (*Device, error) {
	if arg != "" {
		if fi, err := os.Stat(arg); err == nil && !fi.IsDir() {
			b, err := os.ReadFile(arg)
			if err == nil && strings.Contains(string(b[:min(len(b), 4096)]), "\"capabilities\"") {
				d, err := ParseVulkanInfo(b)
				if d != nil {
					d.Source = "vulkaninfo-json:" + filepath.Base(arg)
				}
				return d, err
			}
		}
	}
	icd := arg
	if fi, err := os.Stat(arg); err == nil && fi.IsDir() {
		m, _ := filepath.Glob(filepath.Join(arg, "share", "vulkan", "icd.d", "*.json"))
		if len(m) == 0 {
			m, _ = filepath.Glob(filepath.Join(arg, "*.json"))
		}
		if len(m) == 0 {
			return nil, fmt.Errorf("no ICD manifest found under %s", arg)
		}
		sort.Strings(m)
		icd = m[0]
	}
	key := "default"
	if icd != "" {
		h := sha1.New()
		h.Write([]byte(icd))
		for _, p := range []string{icd, icdLibrary(icd)} {
			if fi, err := os.Stat(p); err == nil {
				fmt.Fprintf(h, "|%s|%d|%d", p, fi.Size(), fi.ModTime().UnixNano())
			}
		}
		key = hex.EncodeToString(h.Sum(nil))[:16]
	}
	cdir, _ := os.UserCacheDir()
	cpath := filepath.Join(cdir, "gpuscan", "vulkaninfo-"+key+".json")
	if !noCache {
		if b, err := os.ReadFile(cpath); err == nil {
			if d, err := ParseVulkanInfo(b); err == nil {
				d.Source, d.Cached = "vulkaninfo:"+icdLabel(icd), true
				return d, nil
			}
		}
	}
	tmp, err := os.MkdirTemp("", "gpuscan-vk")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(tmp)
	out := filepath.Join(tmp, "dev.json")
	cmd := exec.Command("vulkaninfo", "--json=0", "-o", out)
	cmd.Dir = tmp
	cmd.Env = os.Environ()
	if icd != "" {
		cmd.Env = append(cmd.Env, "VK_DRIVER_FILES="+icd, "VK_ICD_FILENAMES="+icd)
	}
	if o, err := cmd.CombinedOutput(); err != nil {
		return nil, fmt.Errorf("vulkaninfo failed: %v: %s", err, strings.TrimSpace(string(o)))
	}
	b, err := os.ReadFile(out)
	if err != nil {
		return nil, err
	}
	d, err := ParseVulkanInfo(b)
	if err != nil {
		return nil, err
	}
	d.Source = "vulkaninfo:" + icdLabel(icd)
	if err := os.MkdirAll(filepath.Dir(cpath), 0o755); err == nil {
		_ = os.WriteFile(cpath, b, 0o644)
	}
	return d, nil
}

func icdLabel(icd string) string {
	if icd == "" {
		return "default-loader"
	}
	return filepath.Base(icd)
}

func icdLibrary(manifest string) string {
	b, err := os.ReadFile(manifest)
	if err != nil {
		return ""
	}
	var m struct {
		ICD struct {
			Lib string `json:"library_path"`
		} `json:"ICD"`
	}
	if json.Unmarshal(b, &m) != nil || m.ICD.Lib == "" {
		return ""
	}
	if filepath.IsAbs(m.ICD.Lib) {
		return m.ICD.Lib
	}
	return filepath.Join(filepath.Dir(manifest), m.ICD.Lib)
}
