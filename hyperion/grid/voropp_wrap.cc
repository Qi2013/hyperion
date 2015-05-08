#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <ios>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <iomanip>

#include "voro++/voro++.hh"

extern "C" const char * hyperion_voropp_wrap(int **neighbours, int *max_nn, double **volumes, double **bb_min, double **bb_max, double **vertices,
                                             int *max_nv, double xmin, double xmax, double ymin, double ymax, double zmin, double zmax,
                                             double const *points, int npoints, int with_vertices, const char *wall_str, const double *wall_args_arr, int n_wall_args,
                                             int with_sampling, int n_samples, double **sample_points, int verbose);

using namespace voro;

// Number of average particles per block for good performance,
// determined experimentally.
static const double particle_block = 5.;

// Simple smart pointer that uses std::free to deallocate.
template <typename T>
class ptr_raii
{
    public:
        explicit ptr_raii(T *ptr):m_ptr(ptr) {}
        ~ptr_raii()
        {
            if (m_ptr) {
                std::free(m_ptr);
            }
        }
        T *release()
        {
            T *retval = m_ptr;
            m_ptr = 0;
            return retval;
        }
        T *get()
        {
            return m_ptr;
        }
    private:
        // Remove copy ctor and assignment operator.
        ptr_raii(const ptr_raii &);
        ptr_raii &operator=(const ptr_raii &);
    private:
        T *m_ptr;
};

// Functor to extract the max number of neighbours/vertices.
template <typename T>
static inline bool size_cmp(const std::vector<T> &a, const std::vector<T> &b)
{
    return a.size() < b.size();
}

// Global string used for reporting errors back to Python.
static std::string error_message;

// Wall utilities.
// Need to store this statically as the wall object needs to exist outside the scope
// in which it is created.
static std::auto_ptr<wall> wall_ptr;

// Helper function to add the wall.
static inline void add_walls(container &con,const char *wall_str,const double *wall_args_arr,int n_wall_args,int verbose)
{
    if (verbose) {
        std::cout << "Wall type: " << wall_str << '\n';
        std::cout << "Wall number of args: " << n_wall_args << '\n';
        std::cout << "Wall params: [";
        for (int i = 0; i < n_wall_args; ++i) {
            std::cout << wall_args_arr[i];
            if (i != n_wall_args - 1) {
                std::cout << ',';
            }
        }
        std::cout << "]\n";
    }

    // Allowed walls: 'sphere','cylinder','cone','plane'.
    if (std::strcmp(wall_str,"sphere") == 0) {
        // Some checks.
        if (n_wall_args != 4) {
            throw std::invalid_argument("invalid number of arguments for a 'sphere' wall, exactly 4 are needed");
        }
        if (wall_args_arr[3] <= 0.) {
            throw std::invalid_argument("the radius of a 'sphere' wall must be strictly positive");
        }
        wall_ptr.reset(new wall_sphere(wall_args_arr[0],wall_args_arr[1],wall_args_arr[2],wall_args_arr[3]));
        con.add_wall(wall_ptr.get());
    }
    if (std::strcmp(wall_str,"cylinder") == 0) {
        // Some checks.
        if (n_wall_args != 7) {
            throw std::invalid_argument("invalid number of arguments for a 'cylinder' wall, exactly 7 are needed");
        }
        if (wall_args_arr[6] <= 0.) {
            throw std::invalid_argument("the radius of a 'cylinder' wall must be strictly positive");
        }
        wall_ptr.reset(new wall_cylinder(wall_args_arr[0],wall_args_arr[1],wall_args_arr[2],wall_args_arr[3],wall_args_arr[4],
            wall_args_arr[5],wall_args_arr[6]
        ));
        con.add_wall(wall_ptr.get());
    }
}

// Compute the volume of a tetrahedron.
template <typename It>
static inline double tetra_volume(It v0, It v1, It v2, It v3)
{
    double mat[9];
    for (int i = 0; i < 3; ++i) {
        mat[i] = v1[i]-v0[i];
    }
    for (int i = 0; i < 3; ++i) {
        mat[3+i] = v2[i]-v0[i];
    }
    for (int i = 0; i < 3; ++i) {
        mat[6+i] = v3[i]-v0[i];
    }
    double a = mat[0], b = mat[1], c = mat[2];
    double d = mat[3], e = mat[4], f = mat[5];
    double g = mat[6], h = mat[7], i = mat[8];
    return std::abs(a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g)) / 6.;
}

template <typename Ptr, typename It>
static inline void sample_point_in_tetra(Ptr res,It p0, It p1, It p2, It p3)
{
    double s = std::rand()/(RAND_MAX + 1.0);
    double t = std::rand()/(RAND_MAX + 1.0);
    double u = std::rand()/(RAND_MAX + 1.0);

    if (s + t > 1.0) {
        s = 1.0 - s;
        t = 1.0 - t;
    }

    if (t + u > 1.0) {
        double tmp = u;
        u = 1.0 - s - t;
        t = 1.0 - tmp;
    } else if (s + t + u > 1.0) {
        double tmp = u;
        u = s + t + u - 1.0;
        s = 1 - t - tmp;
    }

    const double a = 1.0 - s - t - u;

    for (int i = 0; i < 3; ++i) {
        res[i] = p0[i]*a+p1[i]*s+p2[i]*t+p3[i]*u;
    }
}

// Main wrapper called from cpython.
const char *hyperion_voropp_wrap(int **neighbours, int *max_nn, double **volumes, double **bb_min, double **bb_max, double **vertices,
                                 int *max_nv, double xmin, double xmax, double ymin, double ymax, double zmin, double zmax, double const *points,
                                 int nsites, int with_vertices, const char *wall_str, const double *wall_args_arr, int n_wall_args, int with_sampling, int n_samples,
                                 double **sample_points, int verbose)
{
std::cout << std::setprecision(16);
    // We need to wrap everything in a try/catch block as exceptions cannot leak out to C.
    try {

    // Total number of blocks we want.
    const double nblocks = nsites / particle_block;

    // Average block edge.
    const double block_edge = cbrt(nblocks);

    // Average edge length of the domain.
    const double vol_edge = cbrt((xmax - xmin) * (ymax - ymin) * (zmax - zmin));

    // The number of grid blocks across each coordinate will be proportional
    // to the dimension of the domain in that coordinate. The +1 is to account for rounding
    // and to make sure that we always have at least 1 block.
    const int nx = (int)((xmax - xmin) / vol_edge * block_edge) + 1;
    const int ny = (int)((ymax - ymin) / vol_edge * block_edge) + 1;
    const int nz = (int)((zmax - zmin) / vol_edge * block_edge) + 1;

    if (verbose) {
        std::cout << "Number of sites: " << nsites << '\n';
        std::cout << "Domain: [" << xmin << ',' << xmax << "] [" << ymin << ',' << ymax << "] [" << zmin << ',' << zmax << "]\n";
        std::cout << "Initialising with the following block grid: " << nx << ',' << ny << ',' << nz << '\n';
        std::cout << std::boolalpha;
        std::cout << "Vertices: " << bool(with_vertices) << '\n';
    }

    // Prepare the output quantities.
    // Neighbour list.
    std::vector<std::vector<int> > n_list(nsites);
    // List of vertices.
    std::vector<std::vector<double> > vertices_list;
    if (with_vertices) {
        vertices_list.resize(nsites);
    }
    // Volumes.
    ptr_raii<double> vols(static_cast<double *>(std::malloc(sizeof(double) * nsites)));
    // Bounding boxes.
    ptr_raii<double> bb_m(static_cast<double *>(std::malloc(sizeof(double) * nsites * 3)));
    ptr_raii<double> bb_M(static_cast<double *>(std::malloc(sizeof(double) * nsites * 3)));
    // Sampling points.
    ptr_raii<double> spoints(with_sampling ? static_cast<double *>(std::malloc(sizeof(double) * nsites * n_samples * 3)) : NULL);

    // Initialise the voro++ container.
    container con(xmin,xmax,ymin,ymax,zmin,zmax,nx,ny,nz,
                  false,false,false,8);
    for(int i = 0; i < nsites; ++i) {
            con.put(i,points[i*3],points[i*3 + 1],points[i*3 + 2]);
    }

    // Handle the walls.
    add_walls(con,wall_str,wall_args_arr,n_wall_args,verbose);

    // Initialise the looping variables and the temporary cell object used for computation.
    voronoicell_neighbor c;
    c_loop_all vl(con);
    int idx;
    double tmp_min[3],tmp_max[3];
    // Vector to store temporarily the list of vertices coordinates triplets. Used only if vertices
    // are not requested (otherwise, the vertices are stored directly in the output array).
    std::vector<double> tmp_v;
    // List of faces for each cell. Format described here:
    // http://math.lbl.gov/voro++/examples/polygons/
    std::vector<int> f_vert;
    // List of tetrahedra vertices indices: 4 elements per tet.
    std::vector<int> t_vert;
    // Cumulative volumes of the tetrahedra.
    std::vector<double> c_vol;
    // Site position and radius (r is unused).
    double x,y,z,r;

    // Loop over all particles and compute the desired quantities.
    if(vl.start()) {
        do {
            // Get the id and position of the site being considered.
            vl.pos(idx,x,y,z,r);
            std::vector<double> *tmp_vertices = with_vertices ? &(vertices_list[idx]) : &tmp_v;
            // Compute the voronoi cell.
            con.compute_cell(c,vl);
            // Compute the neighbours.
            c.neighbors(n_list[idx]);
            // Volume.
            vols.get()[idx] = c.volume();
            // Compute bounding box. Start by asking for the vertices.
            c.vertices(x,y,z,*tmp_vertices);
            // Init min/max bb.
            std::copy(tmp_vertices->begin(),tmp_vertices->begin() + 3,tmp_min);
            std::copy(tmp_vertices->begin(),tmp_vertices->begin() + 3,tmp_max);
            for (unsigned long i = 1u; i < tmp_vertices->size() / 3u; ++i) {
                for (unsigned j = 0; j < 3; ++j) {
                    if ((*tmp_vertices)[i * 3 + j] < tmp_min[j]) {
                        tmp_min[j] = (*tmp_vertices)[i * 3 + j];
                    }
                    if ((*tmp_vertices)[i * 3 + j] > tmp_max[j]) {
                        tmp_max[j] = (*tmp_vertices)[i * 3 + j];
                    }
                }
            }
            // Copy the bounding box into the output array.
            std::copy(tmp_min,tmp_min + 3,bb_m.get() + idx * 3);
            std::copy(tmp_max,tmp_max + 3,bb_M.get() + idx * 3);
            // Computation of the sampling array, only if requested.
            if (!with_sampling) {
                continue;
            }
            // Clear tmp variables.
            t_vert.clear();
            c_vol.clear();
            double vol = 0;
            // Compute the faces of the cell.
            c.face_vertices(f_vert);
            int j = 0;
            while (j < f_vert.size()) {
                // Number of vertices for each face.
                const int nfv = f_vert[j];
                // We need to establish if the vertex with index 0 in the cell is
                // part of the current face. If that is the case, we skip
                // the face.
                if (std::find(f_vert.begin() + j + 1,f_vert.begin() + j + 1 + nfv,0) != f_vert.begin() + j + 1 + nfv) {
                    // Don't forget to update the counter...
                    j += nfv + 1;
                    continue;
                }
                // Now we need to build the list of tetrahedra vertices. The procedure:
                // - the first vertex is always the 0 vertex of the cell,
                // - the second vertex is always the first vertex of the current face.
                // - the other two vertices are taken as the k-th and k+1-th vertices of
                //   the face.
                for (int k = j + 2; k < j + nfv; ++k) {
                    t_vert.push_back(0);
                    t_vert.push_back(f_vert[j+1]);
                    t_vert.push_back(f_vert[k]);
                    t_vert.push_back(f_vert[k+1]);
                    // Volume of the tetrahedron.
                    double t_vol = tetra_volume(tmp_vertices->begin(),tmp_vertices->begin() + f_vert[j+1]*3,tmp_vertices->begin() + f_vert[k]*3,
                                                tmp_vertices->begin() + f_vert[k+1]*3);
                    // Update the cumulative volume and add it.
                    vol += t_vol;
                    c_vol.push_back(vol);
                }
                // Update the counter.
                j += nfv + 1;
            }
            // Now we need to select randomly tetras (with a probability proportional to their volume) and sample
            // uniformly inside them.
            const double c_factor = c_vol.back()/(RAND_MAX + 1.0);
            for (int i = 0; i < n_samples;) {
                const double r_vol = std::rand()*c_factor;
                std::vector<double>::iterator it = std::upper_bound(c_vol.begin(),c_vol.end(),r_vol);
                // It might be possible due to floating point madness that this actually goes past the end,
                // in such a case we just repeat the sampling.
                if (it == c_vol.end()) {
                    continue;
                }
                int t_idx = std::distance(c_vol.begin(),it);
                // Now we can go sample inside the selected tetrahedron.
                std::vector<int>::iterator t_it = t_vert.begin() + t_idx*4;
                sample_point_in_tetra(spoints.get()+idx*n_samples*3+i*3,tmp_vertices->begin()+(*t_it)*3,tmp_vertices->begin()+(*(t_it + 1))*3,
                                      tmp_vertices->begin()+(*(t_it + 2))*3,tmp_vertices->begin()+(*(t_it + 3))*3);
                ++i;
            }
        } while(vl.inc());
    }

    // The voro++ doc say that in case of numerical errors the neighbours list might not be symmetric,
    // that is, if 'a' is a neighbour of 'b' then 'b' might not be a neighbour of 'a'. We check and fix this
    // in the loop below.
    for (idx = 0; idx < nsites; ++idx) {
        for (unsigned j = 0u; j < n_list[idx].size(); ++j) {
            // Check only non-wall neighbours.
            if (n_list[idx][j] >= 0 && std::find(n_list[n_list[idx][j]].begin(),n_list[n_list[idx][j]].end(),idx) == n_list[n_list[idx][j]].end()) {
                n_list[n_list[idx][j]].push_back(idx);
            }
        }
    }

    // Compute the max number of neighbours.
    *max_nn = std::max_element(n_list.begin(),n_list.end(),size_cmp<int>)->size();
    if (verbose) std::cout << "Max number of neighbours is: " << *max_nn << '\n';

    // Allocate space for flat array of neighbours.
    ptr_raii<int> neighs(static_cast<int *>(std::malloc(sizeof(int) * nsites * (*max_nn))));
    // Fill it in.
    for (idx = 0; idx < nsites; ++idx) {
        int *ptr = neighs.get() + (*max_nn) * idx;
        std::copy(n_list[idx].begin(),n_list[idx].end(),ptr);
        // Fill empty elements with -10.
        std::fill(ptr + n_list[idx].size(),ptr + (*max_nn),-10);
    }

    if (with_vertices) {
        // Compute the max number of vertices coordinates.
        *max_nv = std::max_element(vertices_list.begin(),vertices_list.end(),size_cmp<double>)->size();
        if (verbose) std::cout << "Max number of vertices coordinates is: " << *max_nv << '\n';

        // Allocate space for flat array of vertices.
        ptr_raii<double> verts(static_cast<double *>(std::malloc(sizeof(double) * nsites * (*max_nv))));
        // Fill it in.
        for (idx = 0; idx < nsites; ++idx) {
            double *ptr = verts.get() + (*max_nv) * idx;
            std::copy(vertices_list[idx].begin(),vertices_list[idx].end(),ptr);
            // Fill empty elements with nan.
            std::fill(ptr + vertices_list[idx].size(),ptr + (*max_nv),std::numeric_limits<double>::quiet_NaN());
        }

        // Assign the output quantity.
        *vertices = verts.release();
    } else {
        *max_nv = 0;
    }

    // Assign the output quantities.
    *volumes = vols.release();
    *bb_min = bb_m.release();
    *bb_max = bb_M.release();
    *neighbours = neighs.release();
    *sample_points = spoints.release();

    return NULL;

    } catch (const std::exception &e) {
        error_message = std::string("A C++ exception was raised while calling the voro++ wrapper. The full error message is: \"") + e.what() + "\".";
        return error_message.c_str();
    }
}
