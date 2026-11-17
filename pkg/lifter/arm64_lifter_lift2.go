package lifter

import "strings"

// The m3-lift2 lifter options (docs/m3/lift2.md). Each is global like the liftmisc ones, on by
// default, and an "off" lowering is byte-identical to the one before the option existed.
var (
	roLoads     = true // -ro-loads: constant-address loads from read-only image memory are plain
	redZoneLeaf = true // -redzone-leaf: the red zone of a call-entered function is private under -tso-elide stack
	bmiPeep     = true // -bmi-peep: BMI and narrow-flag peepholes
)

// SetROLoads turns plain loads from read-only image memory on or off (`-ro-loads`).
func SetROLoads(on bool) { roLoads = on }

// GetROLoads reports whether constant loads from read-only image memory are plain.
func GetROLoads() bool { return roLoads }

// SetRedZoneLeaf turns the leaf-function red zone rule of the stack privacy pass on or off (`-redzone-leaf`).
func SetRedZoneLeaf(on bool) { redZoneLeaf = on }

// GetRedZoneLeaf reports whether the leaf red zone rule is on.
func GetRedZoneLeaf() bool { return redZoneLeaf }

// SetBMIPeep turns the BMI and narrow-flag peepholes on or off (`-bmi-peep`).
func SetBMIPeep(on bool) { bmiPeep = on }

// GetBMIPeep reports whether the BMI and narrow-flag peepholes are on.
func GetBMIPeep() bool { return bmiPeep }

// lift2Key is appended to ConfigKey. A switched-off option adds nothing, so the key of the
// all-off configuration is the one it was before these options existed.
func lift2Key(elide bool) string {
	var b strings.Builder
	if roLoads {
		b.WriteString(" roload=1")
	}
	if redZoneLeaf && elide {
		b.WriteString(" rzleaf=1")
	}
	if bmiPeep {
		b.WriteString(" bmi=1")
	}
	return b.String()
}
