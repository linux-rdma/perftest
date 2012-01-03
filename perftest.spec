Name:           perftest
Summary:        IB Performance tests
Version:        1.3.0
Release:        3.2
License:        BSD 3-Clause, GPL v2 or later
Group:          Productivity/Networking/Diagnostic
Source:         http://www.openfabrics.org/downloads/%{name}-%{version}.tar.gz
Url:            http://www.openfabrics.org
BuildRoot:      %{_tmppath}/%{name}-%{version}-build
BuildRequires:  libibverbs-devel librdmacm-devel

%description
gen2 uverbs microbenchmarks



%prep
%setup -q

%build
export CFLAGS="$RPM_OPT_FLAGS"
%{__make}
chmod -x runme

%install
install -D -m 0755 rdma_lat $RPM_BUILD_ROOT%{_bindir}/rdma_lat
install -D -m 0755 rdma_bw $RPM_BUILD_ROOT%{_bindir}/rdma_bw
install -D -m 0755 ib_write_lat $RPM_BUILD_ROOT%{_bindir}/ib_write_lat
install -D -m 0755 ib_write_bw $RPM_BUILD_ROOT%{_bindir}/ib_write_bw
install -D -m 0755 ib_send_lat $RPM_BUILD_ROOT%{_bindir}/ib_send_lat
install -D -m 0755 ib_send_bw $RPM_BUILD_ROOT%{_bindir}/ib_send_bw
install -D -m 0755 ib_read_lat $RPM_BUILD_ROOT%{_bindir}/ib_read_lat
install -D -m 0755 ib_read_bw $RPM_BUILD_ROOT%{_bindir}/ib_read_bw
install -D -m 0755 ib_atomic_lat $RPM_BUILD_ROOT%{_bindir}/ib_atomic_lat
install -D -m 0755 ib_atomic_bw $RPM_BUILD_ROOT%{_bindir}/ib_atomic_bw
install -D -m 0755 ib_write_bw_postlist $RPM_BUILD_ROOT%{_bindir}/ib_write_bw_postlist
install -D -m 0755 ib_clock_test $RPM_BUILD_ROOT%{_bindir}/ib_clock_test

%clean
rm -rf ${RPM_BUILD_ROOT}

%files
%defattr(-, root, root)
%doc README COPYING runme
%_bindir/*

%changelog
* Mon Jan 01 2012 - idos@mellanox.com
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
