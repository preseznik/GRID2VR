// Adapted from DiRT2VR tools/nordschleife-converter/Tiles.cs (a617f7a), same author.
using System.Numerics;

namespace GRID2VR.Tracks;

internal static class Tiles
{
    internal const int NativeLimit=200;
    internal static (List<List<Mesh>> Parts,int Size) Partition(IEnumerable<Mesh> meshes,int? fixedCount=null)
    {
        int size=128;Dictionary<Cell,List<Mesh>> partition;
        while(true)
        {
            partition=[];
            foreach(var mesh in meshes)
            {
                var buckets=new Dictionary<Cell,List<ushort>>();
                for(int i=0;i<mesh.Indices.Length;i+=3)
                {
                    var centre=mesh.DetailAnchor??(mesh.Positions[mesh.Indices[i]]+mesh.Positions[mesh.Indices[i+1]]+mesh.Positions[mesh.Indices[i+2]])/3;
                    var cell=new Cell((int)MathF.Floor(centre.X/size),(int)MathF.Floor(centre.Z/size));
                    if(!buckets.TryGetValue(cell,out var indices))buckets[cell]=indices=[];
                    indices.AddRange([mesh.Indices[i],mesh.Indices[i+1],mesh.Indices[i+2]]);
                }
                foreach(var (cell,indices) in buckets)
                {
                    if(!partition.TryGetValue(cell,out var list))partition[cell]=list=[];
                    list.Add(AcSource.Compact(mesh,indices) with{DetailAnchor=null});
                }
            }
            if(partition.Count<=(fixedCount??NativeLimit))break;
            size*=2;if(size>16384)throw new InvalidDataException("Scene cannot fit the native terrain tile capacity.");
        }
        var parts=partition.OrderBy(p=>p.Key.X).ThenBy(p=>p.Key.Z).Select(p=>p.Value).ToList();
        if(parts.Count==0 || parts.Count>NativeLimit)throw new InvalidDataException("Unsupported spatial tile count.");
        while(fixedCount is not null && parts.Count<fixedCount)
        {
            int part=Enumerable.Range(0,parts.Count).OrderByDescending(i=>parts[i].Sum(m=>m.Indices.Length)).First();
            var faces=parts[part].SelectMany(m=>Enumerable.Range(0,m.Indices.Length/3).Select(i=>
                (Mesh:m,At:i*3,Centre:(m.Positions[m.Indices[i*3]]+m.Positions[m.Indices[i*3+1]]+m.Positions[m.Indices[i*3+2]])/3))).ToArray();
            if(faces.Length<2)throw new InvalidDataException("Not enough geometry for the donor tile inventory.");
            var min=faces.Select(f=>f.Centre).Aggregate(Vector3.Min);var max=faces.Select(f=>f.Centre).Aggregate(Vector3.Max);
            int axis=max.X-min.X>=max.Z-min.Z?0:2;faces=faces.OrderBy(f=>f.Centre[axis]).ToArray();
            List<Mesh> Half(int start,int count)=>faces.Skip(start).Take(count).GroupBy(f=>f.Mesh).Select(g=>AcSource.Compact(g.Key,g.SelectMany(f=>g.Key.Indices.AsSpan(f.At,3).ToArray()).ToArray())).ToList();
            int middle=faces.Length/2;parts[part]=Half(0,middle);parts.Insert(part+1,Half(middle,faces.Length-middle));
        }
        return(parts,size);
    }

    // Two-level split for long courses: triangles near the driving line go into fine
    // cells (stock GRID 2 tiles are ~100-250 m, so HIGH/LOW switching works per corner),
    // distant backdrop into coarse cells. The fine size doubles until the total fits.
    internal static (List<List<Mesh>> Parts,int NearSize,int FarSize) PartitionNearFar(IReadOnlyList<Mesh> meshes,Func<Vector3,bool> near,int farSize=2048)
    {
        for(int size=256;size<=farSize;size*=2)
        {
            var cells=new Dictionary<(bool Near,Cell Cell),List<Mesh>>();
            foreach(var mesh in meshes)
            {
                var buckets=new Dictionary<(bool,Cell),List<ushort>>();
                for(int i=0;i<mesh.Indices.Length;i+=3)
                {
                    var centre=(mesh.Positions[mesh.Indices[i]]+mesh.Positions[mesh.Indices[i+1]]+mesh.Positions[mesh.Indices[i+2]])/3;
                    bool isNear=near(centre);int s=isNear?size:farSize;
                    var key=(isNear,new Cell((int)MathF.Floor(centre.X/s),(int)MathF.Floor(centre.Z/s)));
                    if(!buckets.TryGetValue(key,out var indices))buckets[key]=indices=[];
                    indices.AddRange([mesh.Indices[i],mesh.Indices[i+1],mesh.Indices[i+2]]);
                }
                foreach(var (key,indices) in buckets)
                {
                    if(!cells.TryGetValue(key,out var list))cells[key]=list=[];
                    list.Add(AcSource.Compact(mesh,indices));
                }
            }
            if(cells.Count<=NativeLimit)
                return(cells.OrderBy(c=>c.Key.Near?0:1).ThenBy(c=>c.Key.Cell.X).ThenBy(c=>c.Key.Cell.Z).Select(c=>c.Value).ToList(),size,farSize);
        }
        throw new InvalidDataException("Scene cannot fit the native terrain tile capacity.");
    }
}
