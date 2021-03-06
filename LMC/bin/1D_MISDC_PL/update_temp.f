      subroutine update_temp(scal_old,scal_new,aofs,
     &                       alpha,beta_old,beta_new,Rhs,
     &                       dx,dt,be_cn_theta,lo,hi,bc)
      implicit none
      include 'spec.h'
      real*8 scal_old(-2:nfine+1,nscal)
      real*8 scal_new(-2:nfine+1,nscal)
      real*8     aofs(0 :nfine-1,nscal)
      real*8    alpha(0 :nfine-1)
      real*8 beta_old(-1:nfine  ,nscal)
      real*8 beta_new(-1:nfine  ,nscal)
      real*8      Rhs(0 :nfine-1)
      real*8 dx, dt
      real*8 be_cn_theta
      integer lo,hi,bc(2)
      
      real*8  Ymid(Nspec), rho_old, rho_new, cpmix
      real*8  visc(-1:nfine)
      real*8  RWRK
      integer i,n,is, IWRK
      real*8  Tmid


c*************************************************************************
c     Add adv terms to old state prior to doing stuff below
c*************************************************************************
      do i=lo,hi
         scal_new(i,Temp) = scal_old(i,Temp) + dt*aofs(i,Temp)
      enddo

      call set_bc_s(scal_new,lo,hi,bc)

c*************************************************************************
c     Initialize RHS = (1-theta) * [ Div( lambda Grad(T) ) +  
C                                    rho.D.Grad(Y).Grad(h) ]
C     at old time
c*************************************************************************      
C this fn sets ghost cells
      call get_temp_visc_terms(scal_old,beta_old,visc,dx,lo,hi)
      do i=lo,hi
         Rhs(i) = (1.d0 - be_cn_theta)*dt*visc(i)  
      enddo

c*************************************************************************
c     Add rho.D.Grad(Y).Grad(H)  at time n+1
c*************************************************************************      
      call gamma_dot_gradh(scal_new,beta_new,visc,dx,lo,hi)
      do i=lo,hi
         Rhs(i) = Rhs(i) + dt*be_cn_theta*visc(i) 
      enddo

c*************************************************************************
c     Construct alpha from half-time level data
c     (here, build rho to ensure correct extract of Y regardless of whether
c     rho was updated independently - ie, assume no reset_rho_in_rho_states))
c*************************************************************************

      do i=lo,hi
         rho_old = 0.d0
         rho_new = 0.d0
         do n = 1,Nspec
            is = FirstSpec + n - 1
            rho_old = rho_old + scal_old(i,is)
            rho_new = rho_new + scal_new(i,is)
         enddo
         do n = 1,Nspec
            is = FirstSpec + n - 1
            Ymid(n) =
     &           0.5d0*(scal_old(i,is)/rho_old + scal_new(i,is)/rho_new)
         enddo
         Tmid = 0.5d0*(scal_old(i,Temp) + scal_new(i,Temp))
         call CKCPBS(Tmid,Ymid,IWRK,RWRK,cpmix)
         alpha(i) = 0.5d0 * (rho_old + scal_new(i,Density)) * cpmix
         Rhs(i) = Rhs(i) + scal_new(i,Temp)*alpha(i)
      enddo

      end
      
