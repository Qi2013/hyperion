import hashlib

import h5py
import numpy as np

from hyperion.util.meshgrid import meshgrid_nd
from hyperion.util.functions import FreezableClass, is_numpy_array, monotonically_increasing, link_or_copy
from hyperion.util.logger import logger


class CylindricalPolarGrid(FreezableClass):

    def __init__(self, *args):

        self.shape = None

        self.w_wall = None
        self.z_wall = None
        self.p_wall = None

        self.w = None
        self.z = None
        self.p = None

        self.gw = None
        self.gz = None
        self.gp = None

        self.volumes = None
        self.areas = None
        self.widths = None

        self.quantities = {}

        self._freeze()

        if len(args) > 0:
            self.set_walls(*args)

    def set_walls(self, w_wall, z_wall, p_wall):

        if type(w_wall) in [list, tuple]:
            w_wall = np.array(w_wall)
        if type(z_wall) in [list, tuple]:
            z_wall = np.array(z_wall)
        if type(p_wall) in [list, tuple]:
            p_wall = np.array(p_wall)

        if not is_numpy_array(w_wall) or w_wall.ndim != 1:
            raise ValueError("w_wall should be a 1-D sequence")
        if not is_numpy_array(z_wall) or z_wall.ndim != 1:
            raise ValueError("z_wall should be a 1-D sequence")
        if not is_numpy_array(p_wall) or p_wall.ndim != 1:
            raise ValueError("p_wall should be a 1-D sequence")

        if not monotonically_increasing(w_wall):
            raise ValueError("w_wall should be monotonically increasing")
        if not monotonically_increasing(z_wall):
            raise ValueError("z_wall should be monotonically increasing")
        if not monotonically_increasing(p_wall):
            raise ValueError("p_wall should be monotonically increasing")

        # Find grid shape
        self.shape = (len(p_wall) - 1, len(z_wall) - 1, len(w_wall) - 1)

        # Store wall positions
        self.w_wall = w_wall
        self.z_wall = z_wall
        self.p_wall = p_wall

        # Compute cell centers
        self.w = 10. ** ((np.log10(w_wall[:-1]) + np.log10(w_wall[1:])) / 2.)
        if w_wall[0] == 0.:
            self.w[0] = w_wall[1] / 2.
        self.z = (z_wall[:-1] + z_wall[1:]) / 2.
        self.p = (p_wall[:-1] + p_wall[1:]) / 2.

        # Generate 3D versions of r, t, p
        #(each array is 3D and defined in every cell)
        self.gw, self.gz, self.gp = meshgrid_nd(self.w, self.z, self.p)

        # Generate 3D versions of the inner and outer wall positions respectively
        gw_wall_min, gz_wall_min, gp_wall_min = \
                    meshgrid_nd(w_wall[:-1], z_wall[:-1], p_wall[:-1])

        gw_wall_max, gz_wall_max, gp_wall_max = \
                    meshgrid_nd(w_wall[1:], z_wall[1:], p_wall[1:])

        # USEFUL QUANTITIES

        dr = gw_wall_max - gw_wall_min
        dr2 = gw_wall_max ** 2 - gw_wall_min ** 2
        dz = gz_wall_max - gz_wall_min
        dp = gp_wall_max - gp_wall_min

        # CELL VOLUMES

        #   dV = dr * dz * (r*dphi)
        #    V = [r_2^2 - r_1^2] / 2. * [z_2 - z_1] * [phi_2 - phi_1]

        self.volumes = dr2 * dz * dp / 2.

        # WALL AREAS

        self.areas = np.zeros((6,) + self.shape)

        # R walls:
        #   dA = r * dz * dphi
        #    A = r * [z 2 - z_1] * [phi_2 - phi_1]

        self.areas[0, :, :, :] = gw_wall_min * dz * dp
        self.areas[1, :, :, :] = gw_wall_max * dz * dp

        # z walls:
        #   dA = r * dr * dphi
        #    A = 0.5 * [r_2^2 - r_1^2] * [phi_2 - phi_1]

        self.areas[2, :, :, :] = 0.5 * dr2 * dp
        self.areas[3, :, :, :] = 0.5 * dr2 * dp

        # Phi walls:
        #   dA = dr * dz
        #    A = [r_2 - r_1] * [z_2 - z_1]

        self.areas[4, :, :, :] = dr * dz
        self.areas[5, :, :, :] = dr * dz

        # CELL WIDTHS

        self.widths = np.zeros((3,) + self.shape)

        # R direction:
        #   dS = dr
        #    S = r_2 - r_1

        self.widths[0, :, :, :] = dr

        # z direction:
        #   dS = dz
        #    S = [z_2 - z_1]

        self.widths[1, :, :, :] = dz

        # Phi direction:
        #   dS = r * dphi
        #    S = r * [phi_2 - phi_1]

        self.widths[2, :, :, :] = self.gw * dp

    def __getattr__(self, attribute):
        if attribute == 'n_dust':
            n_dust = None
            for quantity in self.quantities:
                if type(self.quantities[quantity]) in [list, tuple]:
                    if n_dust is None:
                        n_dust = len(self.quantities[quantity])
                    else:
                        if n_dust != len(self.quantities[quantity]):
                            raise ValueError("Not all dust lists in the grid have the same size")
            return n_dust
        else:
            return FreezableClass.__getattribute__(self, attribute)

    def _check_array_dimensions(self, array=None):
        '''
        Check that a grid's array dimensions agree with this grid's metadata

        Parameters
        ----------
        array: np.ndarray or list of np.ndarray, optional
            The array for which to test the dimensions. If this is not
            specified, this method performs a self-consistency check of array
            dimensions and meta-data.
        '''

        for quantity in self.quantities:

            array = self.quantities[quantity]

            if type(array) in [list, tuple]:

                # Check that dimensions are compatible
                for item in array:
                    if item.shape != self.shape:
                        raise ValueError("Arrays in list do not have the right "
                                         "dimensions: %s instead of %s"
                                         % (item.shape, self.shape))

            elif type(array) == np.ndarray:

                if array.shape != self.shape:
                    raise ValueError("Array does not have the right "
                                     "dimensions: %s instead of %s"
                                     % (array.shape, self.shape))

            elif isinstance(array, h5py.ExternalLink):

                array = h5py.File(array.filename, 'r')[array.path]

                if len(array.shape) == 3:

                    if array.shape != self.shape:
                        raise ValueError("Array does not have the right "
                                         "dimensions: %s instead of %s"
                                         % (array.shape, self.shape))

                elif len(array.shape) == 4:

                    for item in array:
                        if item.shape != self.shape:
                            raise ValueError("Arrays in list do not have the right "
                                             "dimensions: %s instead of %s"
                                             % (item.shape, self.shape))

                else:

                    raise Exception("Unexpected number of dimensions: %i" % array.ndim)

            else:

                raise ValueError("Array should be a list or a Numpy array")

    def read(self, group, quantities='all'):
        '''
        Read in a cylindrical polar grid

        Parameters
        ----------
        group: h5py.Group
            The HDF5 group to read the grid from
        quantities: 'all' or list
            Which physical quantities to read in. Use 'all' to read in all
            quantities or a list of strings to read only specific quantities.
        '''

        # Extract HDF5 groups for geometry and physics

        g_geometry = group['Geometry']
        g_quantities = group['Quantities']

        # Read in geometry

        if g_geometry.attrs['grid_type'] != 'cyl_pol':
            raise ValueError("Grid is not cylindrical polar")

        self.set_walls(g_geometry['Walls 1']['w'],
                       g_geometry['Walls 2']['z'],
                       g_geometry['Walls 3']['p'])

        # Check that advertised hash matches real hash
        if g_geometry.attrs['geometry'] != self.get_geometry_id():
            raise Exception("Calculated geometry hash does not match hash in file")

        # Read in physical quantities

        for quantity in g_quantities:
            if quantities == 'all' or quantity in quantities:
                # TODO - if array is 4D, need to convert to list
                self.quantities[quantity] = np.array(g_quantities[quantity].array)

    def write(self, group, quantities='all', copy=True, absolute_paths=False, compression=True, wall_dtype=float, physics_dtype=float):
        '''
        Write out the cylindrical polar grid

        Parameters
        ----------
        group: h5py.Group
            The HDF5 group to write the grid to
        quantities: 'all' or list
            Which physical quantities to write out. Use 'all' to write out all
            quantities or a list of strings to write only specific quantities.
        compression: bool
            Whether to compress the arrays in the HDF5 file
        wall_dtype: type
            The datatype to use to write the wall positions
        physics_dtype: type
            The datatype to use to write the physical quantities
        '''

        # Create HDF5 groups if needed

        if 'Geometry' not in group:
            g_geometry = group.create_group('Geometry')
        else:
            g_geometry = group['Geometry']

        if 'Quantities' not in group:
            g_quantities = group.create_group('Quantities')
        else:
            g_quantities = group['Quantities']

        # Write out geometry

        g_geometry.attrs['grid_type'] = 'cyl_pol'
        g_geometry.attrs['geometry'] = self.get_geometry_id()

        dset = g_geometry.create_dataset("Walls 1", data=np.array(zip(self.w_wall), dtype=[('w', wall_dtype)]), compression=compression)
        dset.attrs['Unit'] = 'cm'

        dset = g_geometry.create_dataset("Walls 2", data=np.array(zip(self.z_wall), dtype=[('z', wall_dtype)]), compression=compression)
        dset.attrs['Unit'] = 'cm'

        dset = g_geometry.create_dataset("Walls 3", data=np.array(zip(self.p_wall), dtype=[('p', wall_dtype)]), compression=compression)
        dset.attrs['Unit'] = 'cm'

        # Self-consistently check geometry and physical quantities
        self._check_array_dimensions()

        # Write out physical quantities

        for quantity in self.quantities:
            if quantities == 'all' or quantity in quantities:
                if isinstance(self.quantities[quantity], h5py.ExternalLink):
                    link_or_copy(g_quantities, quantity, self.quantities[quantity], copy, absolute_paths=absolute_paths)
                else:
                    dset = g_quantities.create_dataset(quantity, data=self.quantities[quantity],
                                                    compression=compression,
                                                    dtype=physics_dtype)
                    dset.attrs['geometry'] = self.get_geometry_id()

    def get_geometry_id(self):
        geo_hash = hashlib.md5()
        geo_hash.update(self.w_wall)
        geo_hash.update(self.z_wall)
        geo_hash.update(self.p_wall)
        return geo_hash.hexdigest()

    def __getitem__(self, item):
        return CylindricalPolarGridView(self, item)

    def __setitem__(self, item, value):
        if isinstance(value, CylindricalPolarGridView):
            if self.x_wall is None and self.y_wall is None and self.z_wall is None:
                logger.warn("No geometry in target grid - copying from original grid")
                self.set_walls(value.x_wall, value.y_wall, value.z_wall)
            self.quantities[item] = value.quantities[value.viewed_quantity]
        elif isinstance(value, h5py.ExternalLink):
            self.quantities[item] = value
        elif value == []:
            self.quantities[item] = []
        else:
            raise ValueError('value should be an empty list, and ExternalLink, or a CylindricalPolarGridView instance')

    def __contains__(self, item):
        return self.quantities.__contains__(item)


class CylindricalPolarGridView(CylindricalPolarGrid):

    def __init__(self, grid, quantity):
        self.viewed_quantity = quantity
        CylindricalPolarGrid.__init__(self)
        self.set_walls(grid.w_wall, grid.z_wall, grid.p_wall)
        self.quantities = {quantity: grid.quantities[quantity]}

    def append(self, grid):
        '''
        Used to append quantities from another grid

        Parameters
        ----------
        grid: 3D Numpy array or CylindricalPolarGridView instance
            The grid to copy the quantity from
        '''
        if isinstance(grid, CylindricalPolarGridView):
            self.quantities[self.viewed_quantity].append(grid.quantities[grid.viewed_quantity])
        elif type(grid) is np.ndarray:
            self.quantities[self.viewed_quantity].append(grid)
        else:
            raise ValueError("grid should be a Numpy array or a CylindricalPolarGridView object")
