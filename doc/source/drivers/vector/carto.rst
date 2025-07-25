.. _vector.carto:

================================================================================
Carto
================================================================================

.. shortname:: CARTO

.. build_dependencies:: libcurl

This driver can connect to the services implementing the Carto API. GDAL/OGR
must be built with Curl support in order for the Carto driver to be compiled.

The driver supports read and write operations.

Driver capabilities
-------------------

.. supports_create::

.. supports_georeferencing::

Dataset name syntax
-------------------

The minimal syntax to open a Carto datasource is :

.. code-block::

   Carto:[connection_name]

For single-user accounts, connection name is the account name. For multi-user
accounts, connection_name must be the user name, not the account name.
Additional optional parameters can be specified after the ':' sign.
Currently the following one is supported:

-  **tables=table_name1[,table_name2]\***: A list of table names. This
   is necessary when you need to access to public tables for example.

If several parameters are specified, they must be separated by a space.

Authentication
--------------

Most operations, in particular write operations, require an authenticated
access. The only exception is read-only access to public tables.

Authenticated access is obtained by specifying the API key given in the
management interface of the Carto service. It is specified with the
CARTO_API_KEY configuration option.

Geometry
--------

The OGR driver will report as many geometry fields as available in the layer
(except the 'the_geom_webmercator' field), following RFC 41.

Filtering
---------

The driver will forward any spatial filter set with
:cpp:func:`OGRLayer::SetSpatialFilter` to the server.
It also makes the same for attribute filters set with
:cpp:func:`OGRLayer::SetAttributeFilter`.

Paging
------

Features are retrieved from the server by chunks of 500 by default. This
number can be altered with the :config:`CARTO_PAGE_SIZE` configuration option.

Write support
-------------

Table creation and deletion is possible.

Write support is only enabled when the datasource is opened in update
mode.

The mapping between the operations of the Carto service and the OGR
concepts is the following :

- :cpp:func:`OGRLayer::CreateFeature` <==> ``INSERT`` operation
- :cpp:func:`OGRLayer::SetFeature` <==> ``UPDATE`` operation
- :cpp:func:`OGRLayer::DeleteFeature` <==> ``DELETE`` operation
- :cpp:func:`GDALDataset::CreateLayer` <==> ``CREATE TABLE`` operation
- :cpp:func:`GDALDataset::DeleteLayer` <==> ``DROP TABLE`` operation

When inserting a new feature with :cpp:func:`OGRLayer::CreateFeature`,
and if the command is successful, OGR will fetch the returned rowid and use it
as the OGR FID.

The above operations are by default issued to the server synchronously with the
OGR API call. This however can cause performance penalties when issuing a lot
of commands due to many client/server exchanges.

So, on a newly created layer, the ``INSERT`` of
:cpp:func:`OGRLayer::CreateFeature` operations are grouped together in chunks
until they reach 15 MB (can be changed with the :config:`CARTO_MAX_CHUNK_SIZE`
configuration option, with a value in MB), at which point they are transferred
to the server. By setting :config:`CARTO_MAX_CHUNK_SIZE` to 0, immediate transfer occurs.

.. warning::

    Don't use :cpp:func:`GDALDataset::DeleteLayer` and
    :cpp:func:`GDALDataset::CreateLayer` to overwrite a table. Instead only
    call :cpp:func:`GDALDataset::CreateLayer` with OVERWRITE=YES. This will
    avoid CARTO deleting maps that depend on this table

SQL
---

SQL commands provided to the :cpp:func:`GDALDataset::ExecuteSQL` call
are executed on the server side, unless the OGRSQL dialect is specified.
You can use the full power of PostgreSQL + PostGIS SQL capabilities.

Open options
------------

The following open options are available:
|about-open-options|

-  .. oo:: BATCH_INSERT
      :choices: YES, NO
      :default: YES

      Whether to group feature insertions in a
      batch. Only apply in creation or update mode.

-  .. oo:: COPY_MODE
      :choices: YES, NO
      :default: YES

      Using COPY for insertions and reads can
      result in a performance improvement.

Layer creation options
----------------------

|about-layer-creation-options|
The following layer creation options are supported:

-  .. lco:: OVERWRITE
      :choices: YES, NO
      :default: NO

      Whether to overwrite an existing table with
      the layer name to be created.

-  .. lco:: GEOMETRY_NULLABLE
      :choices: YES, NO
      :default: YES

      Whether the values of the geometry column can be NULL.

-  .. lco:: CARTODBFY
      :choices: YES, NO
      :default: YES

      Whether the created layer should be
      "Cartodbifi'ed" (i.e. registered in dashboard).
      Requires:

   -  **SRS**: Output SRS must be EPSG:4326. You can use ``-a_srs`` or
      ``-t_srs`` to assign or transform to 4326 before importing.
   -  **Geometry type**: Must be different than NONE. You can set to
      something generic with ``-nlt GEOMETRY``.

-  .. lco:: LAUNDER
      :choices: YES, NO
      :default: YES

      This may be "YES" to force new fields created
      on this layer to have their field names "laundered" into a form more
      compatible with PostgreSQL. This converts to lower case and converts
      some special characters like "-" and "#" to "_". If "NO" exact names
      are preserved. If enabled the table (layer) name will also be laundered.

Configuration options
---------------------

|about-config-options|
The following configuration options are available:

-  .. config:: CARTO_API_URL
      :default: https://[account_name].carto.com/api/v2/sql

      Can be used to point to another server.

-  .. config:: CARTO_HTTPS
      :choices: YES, NO

      can be set to NO to use http:// protocol instead of
      https:// (only if :config:`CARTO_API_URL` is not defined).

-  .. config:: CARTO_MAX_CHUNK_SIZE

-  .. config:: CARTO_API_KEY

      see following paragraph.

-  .. config:: CARTO_PAGE_SIZE
      :default: 500

      features are retrieved from the server by chunks
      of 500 by default. This number can be altered with the configuration option.

Examples
--------

Accessing data from a public table:

.. code-block::

    ogrinfo -ro "Carto:gdalautotest2 tables=tm_world_borders_simpl_0_3"

Creating and populating a table from a shapefile:

.. code-block::

    ogr2ogr --config CARTO_API_KEY abcdefghijklmnopqrstuvw -f Carto "Carto:myaccount" myshapefile.shp

Creating and populating a table from a CSV containing geometries on EPSG:4326:

.. code-block::

    ogr2ogr --config CARTO_API_KEY abcdefghijklmnopqrstuvw -f Carto "Carto:myaccount" file.csv -a_srs 4326 -nlt GEOMETRY

.. note::

    The ``-a_srs`` and ``-nlt`` must be provided to CARTODBFY
    since the information isn't extracted from the CSV.

See Also
--------

-  `Carto API overview <https://carto.com/docs/>`__
