Name:           perftest
Summary:        IB Performance tests
Version:        23.07.0
Release:        0.0
License:        BSD 3-Clause, GPL v2 or later
Group:          Productivity/Networking/Diagnostic
Source:         http://www.openfabrics.org/downloads/%{name}-%{version}.tar.gz
Url:            http://www.openfabrics.org
BuildRoot:      %{_tmppath}/%{name}-%{version}-build
BuildRequires:  libibverbs-devel librdmacm-devel libibumad-devel
BuildRequires:  pciutils-devel

%description
gen3 uverbs microbenchmarks

%prep
%setup -q

%build
%configure \
%if %{?_cuda_h_path:1}0
        CUDA_H_PATH=%{_cuda_h_path}
%endif
%{__make}
chmod -x runme

%install
rm -rf $RPM_BUILD_ROOT
make DESTDIR=%{buildroot} install

%clean
rm -rf ${RPM_BUILD_ROOT}

%files
%defattr(-, root, root)
%doc README COPYING runme
%_bindir/*
%_mandir/man1/*.1*

%changelog
* Wed Jan 09 2013 - idos@mellanox.com
- Use autotools for building package.
* Sun Dec 30 2012 - idos@mellanox.com
- Added raw_ethernet_bw to install script.
* Sun Oct 21 2012 - idos@mellanox.com
- Removed write_bw_postlist (feature contained in all BW tests)
* Sat Oct 20 2012 - idos@mellanox.com
- Version 2.0 is underway
* Mon May 14 2012 - idos@mellanox.com
- Removed (deprecated) rdma_bw and rdma_lat tests
* Thu Feb 02 2012 - idos@mellanox.com
- Updated to 1.4.0 version (no compability with older version).
* Thu Feb 02 2012 - idos@mellanox.com
- Merge perftest code for Linux & Windows
* Sun Jan 01 2012 - idos@mellanox.com
- Added atomic benchmarks
* Sat Apr 18 2009 - hal.rosenstock@gmail.com
- Change executable names for rdma_lat and rdma_bw
* Mon Jul 09 2007 - hvogel@suse.de
- Use correct version
* Wed Jul 04 2007 - hvogel@suse.de
- Add GPL COPYING file [#289509]
* Mon Jul 02 2007 - hvogel@suse.de
- Update to the OFED 1.2 version
* Fri Jun 22 2007 - hvogel@suse.de
- Initial Package, Version 1.1
