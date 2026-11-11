using System.Numerics;

namespace GRID2VR.Tracks;

// A driving-line sample: position, forward tangent, left/right road widths and
// distance along the source spline (metres).
internal sealed record Gate(Vector3 Position, Vector3 Tangent, float Left, float Right, float Distance);
internal readonly record struct Cell(int X, int Z);
