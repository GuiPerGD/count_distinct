%{!?gdcversion: %define gdcversion local}
%global pgmajorversion 95
%global pginstdir /usr/pgsql-9.5
%global sname postgresql-count-distinct

Name:		%{sname}_%{pgmajorversion}
Version:	master
Release:	1.%{gdcversion}
Summary:	Faster implementation of the Count Distinct Function by Tomas Vondra

Group:		Applications/Databases
License:	BSD
URL:		https://github.com/tvondra/count_distinct
Source0:	count-distinct.tar.gz
BuildRoot:	%{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

BuildRequires:	postgresql%{pgmajorversion}-devel
Requires:	postgresql%{pgmajorversion}

%description
This extension provides an alternative to COUNT(DISTINCT ...) which for large amounts of data often ends in sorting and poor performance.

%prep
%setup -q -c -n %{sname}


%build
PATH="%{pginstdir}/bin;$PATH" ; export PATH
CFLAGS="${CFLAGS:-%optflags}" ; export CFLAGS

USE_PGXS=1 make %{?_smp_mflags}

%install
rm -rf %{buildroot}
make USE_PGXS=1 DESTDIR=%{buildroot} install

%clean
rm -rf %{buildroot}

%files
%defattr(644,root,root,755)
%doc README.md
%{pginstdir}/lib/count_distinct.so
%{pginstdir}/share/extension/count_distinct--3.0.0.sql 
%{pginstdir}/share/extension/count_distinct--1.3.1--1.3.2.sql
%{pginstdir}/share/extension/count_distinct--1.3.2--1.3.3.sql 
%{pginstdir}/share/extension/count_distinct--1.3.3--2.0.0.sql
%{pginstdir}/share/extension/count_distinct--2.0.0--3.0.0.sql
%{pginstdir}/share/extension/count_distinct.control

%changelog
* Mon Apr 30 2018 Guilherme Pereira
- Initial draft
